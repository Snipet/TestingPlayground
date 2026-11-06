// Source/plugin/ProcessorAb.cpp: the Processor's A/B listening (EditorPort::ab; Processor.h's AbRun; editor/AbCard.h).
//
// A side plays as a run of its files (Compiler::request): the Engine takes it as any program (the effect crossfades over
// 20 ms, the synth's old voices fade and the held keys start again on it), and the editor's draft stays as it is, so
// what plays and what the editor shows differ until a side is kept. With each side its own dials, they land by name
// when its compile does (a version's are, poll()'s abDialsRequest_), and the side left keeps the dials as the user left
// them there. The loudness is measured offline (AbMatch.h), again once the dials have moved and come to rest, and the
// louder side turned down on the audio thread by the program that plays (trimOutput: keyed by Patch::program, so the
// trim changes the block the program does; a program it does not know, a side's refine or its old program while its
// next compiles, keeps the gain that plays, so nothing is turned up on the way).
//
// A program that lands from anything else (Claude's run, the folder's run on save, a version or an example opened)
// ends it, and so does a session loaded (setStateInformation, abSessionLoaded): that program plays on, nothing
// restored (AbState::ended says so). Keeping a side makes its files the draft (an open editor merges them as one undo
// step) and, a version's, the project open; endAb puts back what played before (and its dials, when each side had its
// own). Until Keep's or Close's compile lands, the side that plays keeps its trim and that program plays untrimmed; a
// session saved meanwhile, or while it is on, keeps what Close would put back (abKept_). Message thread, but chatAb
// (any thread).
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
        constexpr double kRestSeconds = 0.5;                 // the dials still this long: measured again

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

        // The host's values alike (a NaN the same as a NaN: the host's, which plays the last good value).
        bool sameValues(const std::array<float, kMaxControls>& a, const std::array<float, kMaxControls>& b)
        {
            for (std::size_t i = 0; i < a.size(); ++i)
                if (!(a[i] == b[i] || (std::isnan(a[i]) && std::isnan(b[i]))))
                    return false;
            return true;
        }

        // Dials alike for a measure: the same names, each value within 0.01 % (a value set and read back through its
        // parameter's normalised float).
        bool sameDials(const std::map<std::string, float>& a, const std::map<std::string, float>& b)
        {
            if (a.size() != b.size())
                return false;
            for (auto i = a.begin(), j = b.begin(); i != a.end(); ++i, ++j)
                if (i->first != j->first
                    || !(std::fabs(i->second - j->second) <= 1.0e-4f * std::max({ 1.0f, std::fabs(i->second), std::fabs(j->second) })))
                    return false;
            return true;
        }
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
        abSessionLoaded();
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
        std::array<float, kMaxControls> norms{};
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            run.currentFiles = draft_.files;
            run.currentActive = draft_.active;
            run.beforeFiles = nowFiles = running_;
            run.beforeProgram = nowProgram = program_;
        }
        // The request the program that plays answers (a refine of it carries it: abLanded).
        uint64_t nowRequest = compiler_.status().request;
        for (int c = 0; c < kMaxControls; ++c)
            norms[static_cast<std::size_t>(c)] = raw_[static_cast<std::size_t>(c)]->load();
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
        else if (abEnding_ != 0)
        {
            // The last one's Close or Keep still compiles: what plays once it lands is what played before this one, and
            // that compile is this one's own.
            run.beforeFiles = nowFiles = abEndingFiles_;
            run.beforeProgram = nowProgram = nowRequest = abEnding_;
            if (!abEndingDials_.empty())
                run.beforeDials = abEndingDials_;
            run.requests.push_back(abEnding_);
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
        {
            ab_.seenRequest = std::max({ compiler_.status().request, userRun_.load(), dialsRequest_, defaultsRequest_, abEnding_ });
            abEnding_ = 0;
            abEndingFiles_.clear();
            abEndingDials_.clear();
            const std::lock_guard<std::mutex> lock(programMutex_);
            abKept_ = {};
            abKept_.on = true;
            abKept_.run = ab_.beforeFiles;
            abKept_.norms = norms;
        }
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
                ab_.request[static_cast<std::size_t>(k)] = nowRequest;
                ab_.program[static_cast<std::size_t>(k)] = nowProgram;
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
        abSessionLoaded();
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
        // The side that played (or was on its way) is another program now: it plays as such. Its old program, which
        // plays until then, keeps the gain it plays at (trimOutput holds a program it does not know), and the new one
        // takes that side's trim until it is measured.
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

    void Processor::playAb(int side)
    {
        abSessionLoaded();
        abPlay(side, true);
    }

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
        // Its trim published before its compile is offered (Patch::program is its request); the program that plays until
        // then keeps its gain, known or not (trimOutput).
        const uint64_t id = compiler_.request(ab_.st.sides[i].files);
        ab_.requests.push_back(id);
        ab_.request[i] = ab_.program[i] = id;
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
        abSessionLoaded();
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
        {
            // Close puts back the dials of before only while each side has its own: so does a session saved.
            const std::lock_guard<std::mutex> lock(programMutex_);
            abKept_.dials = own;
        }
        ++ab_.st.revision;
        abMeasure();
        abPublish();
    }

    void Processor::setAbMatch(bool on)
    {
        abSessionLoaded();
        if (!ab_.st.on || on == ab_.st.match)
            return;
        ab_.st.match = on;
        // The dials not followed while it was off: measured again when they moved since.
        if (on && !(ab_.measuredWith && sameDials((*ab_.measuredWith)[0], abMeasureDials()[0])
                    && sameDials((*ab_.measuredWith)[1], abMeasureDials()[1])))
            abMeasure();
        ++ab_.st.revision;
        abPublish();
    }

    bool Processor::keepAb(int side, std::string& why)
    {
        abSessionLoaded();
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
        const uint64_t ending = ab_.st.pending == side ? ab_.pendingRequest : 0;   // its compile on its way
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
            if (ending != 0)
            {
                abKept_.run = kept.files;                        // a session saved before it lands keeps it
                abKept_.dials = false;
            }
        }
        if (ending != 0)
        {
            abEndingFiles_ = kept.files;
            abEndingDials_ = ab_.st.ownDials ? ab_.dials[i] : std::vector<projectstore::Dial>{};
        }
        const std::string what = kept.version > 0 ? "version " + std::to_string(kept.version) + " of \"" + ab_.st.project + "\""
                                                  : std::string("the editor's files as they were");
        if (ab_.st.ownDials)
            journal_.dialsNote(Journal::Who::user, std::string("kept ") + letter(side) + " of the A/B listening (" + what + ", its own dials)");
        else
            journal_.note(Journal::Who::user, std::string("kept ") + letter(side) + " of the A/B listening (" + what + ")");
        abStop(ending);
        return true;
    }

    void Processor::endAb()
    {
        abSessionLoaded();
        if (!ab_.st.on)
            return;
        const int playing = ab_.st.playing;
        const bool same = ab_.st.pending < 0 && playing >= 0 && ab_.program[static_cast<std::size_t>(playing)] == ab_.beforeProgram;
        uint64_t ending = 0;
        if (!same && !ab_.beforeFiles.empty())
        {
            ending = compiler_.request(ab_.beforeFiles);
            abEndingFiles_ = ab_.beforeFiles;
            abEndingDials_.clear();
            if (ab_.st.ownDials)
            {
                abDials_ = ab_.beforeDials;
                abDialsRequest_ = ending;
                abEndingDials_ = ab_.beforeDials;
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
        abStop(ending);
    }

    // ---- poll()'s ----------------------------------------------------------------------------------------------------
    void Processor::abLanded(const CompileStatus& s)
    {
        // Close's or Keep's compile landed (or failed, or another came after it): the trims go, and a session saved
        // keeps what plays.
        if (abEnding_ != 0 && s.request >= abEnding_)
        {
            abEnding_ = 0;
            abEndingFiles_.clear();
            abEndingDials_.clear();
            if (!ab_.st.on)
            {
                const std::lock_guard<std::mutex> lock(programMutex_);
                abKept_ = {};
            }
            abPublish();
        }
        if (!ab_.st.on)
            return;
        bool changed = false;
        const bool ours = std::find(ab_.requests.begin(), ab_.requests.end(), s.request) != ab_.requests.end();
        const int playing = ab_.st.playing;
        if (ab_.pendingRequest != 0 && s.request == ab_.pendingRequest)
        {
            // The side's compile, refined or not (a refine's status may be the first poll() sees of it: its program then).
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
                if (s.refined && playing >= 0 && playing != k)   // the side that played plays on, refined since
                    ab_.program[static_cast<std::size_t>(playing)] = s.program;
            }
            changed = true;
        }
        else if (s.request > ab_.seenRequest && !ours)
        {
            if (s.ok)
            {
                abStop();
                ab_.st.ended = "Another program runs now (Claude's, the folder's run on save, a version or an example opened): "
                               "the A/B ended";
                return;
            }
            // A run from elsewhere that failed: the side plays on, and a refine of it since is that side's.
            if (s.refined && playing >= 0 && ab_.program[static_cast<std::size_t>(playing)] != s.program)
            {
                ab_.program[static_cast<std::size_t>(playing)] = s.program;
                changed = true;
            }
        }
        else if (s.refined && s.program != 0)
        {
            // The self-test's refine (Compiler.h): a new program, the same side: the one whose request it answers, or,
            // after a compile that failed, the one that played on.
            int k = -1;
            if (s.ok)
            {
                for (int j = 0; j < 2; ++j)
                    if (ab_.request[static_cast<std::size_t>(j)] == s.request)
                        k = j;
            }
            else
                k = playing;
            if (k >= 0 && ab_.program[static_cast<std::size_t>(k)] != s.program)
            {
                ab_.program[static_cast<std::size_t>(k)] = s.program;
                changed = true;
            }
        }
        if (changed)
        {
            ++ab_.st.revision;
            abPublish();
        }
    }

    void Processor::abSessionLoaded()
    {
        if (abStateLoaded_.exchange(0) == 0)
            return;
        abDials_.clear();                                        // a side's dials on their way: the session's stand
        abDialsRequest_ = 0;
        if (ab_.st.on || abEnding_ != 0)
        {
            const bool was = ab_.st.on;
            abStop();
            if (was)
            {
                ab_.st.ended = "A saved state was loaded (the host's: a session, a preset, an undo), and its program runs now: "
                               "the A/B ended";
                ++ab_.st.revision;
            }
        }
        abLoading_.store(false);
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

    void Processor::abFollowDials()
    {
        if (!ab_.st.on || !ab_.st.match)
            return;
        std::array<float, kMaxControls> raw{};
        for (int c = 0; c < kMaxControls; ++c)
            raw[static_cast<std::size_t>(c)] = raw_[static_cast<std::size_t>(c)]->load();
        const double now = seconds();
        if (!sameValues(raw, ab_.rawSeen))
        {
            ab_.rawSeen = raw;
            ab_.movedAt = now;
            ab_.restless = true;
        }
        if (!ab_.restless || ab_.st.pending >= 0)
            return;                                              // a side on its way: looked at once it lands
        const AbRun::Measured use = abMeasureDials();
        if (ab_.measuredWith && sameDials(use[0], (*ab_.measuredWith)[0]) && sameDials(use[1], (*ab_.measuredWith)[1]))
        {
            ab_.restless = false;                                // where it was measured (a side's own dials landing)
            return;
        }
        if (ab_.measuredWith)
        {
            // Moved from the dials it was measured at: that match is out of date, and says so until measured again.
            abMatcher_.cancel();
            ab_.job = 0;
            ab_.measuredWith.reset();
            ab_.st.measuring = true;
            ++ab_.st.revision;
            abPublish();
        }
        if (now - ab_.movedAt >= kRestSeconds)
        {
            abMeasure();
            ++ab_.st.revision;
            abPublish();
        }
    }

    Processor::AbRun::Measured Processor::abMeasureDials() const
    {
        AbRun::Measured out;
        const std::map<std::string, float> now = byName(dialsNow());
        for (std::size_t k = 0; k < 2; ++k)
        {
            out[k] = now;                                        // the side's own over the plugin's (AbMatch.h)
            if (ab_.st.ownDials && !(static_cast<int>(k) == ab_.st.playing && ab_.st.pending < 0))
                for (const projectstore::Dial& d : ab_.dials[k])
                    out[k][d.name] = d.value;
        }
        return out;
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
        ab_.measuredWith = abMeasureDials();
        for (int c = 0; c < kMaxControls; ++c)
            ab_.rawSeen[static_cast<std::size_t>(c)] = raw_[static_cast<std::size_t>(c)]->load();
        ab_.restless = false;
        const double rate = engine_.rate();
        job.rate = rate >= 8000.0 ? rate : 48000.0;
        ab_.st.measuring = true;
        abMatcher_.start(std::move(job));
    }

    void Processor::abStop(uint64_t ending)
    {
        abMatcher_.cancel();
        ab_.st.on = false;
        ab_.st.pending = -1;
        ab_.st.measuring = false;
        ab_.st.ended.clear();
        ab_.pendingRequest = 0;
        ab_.requests.clear();
        ab_.measuredWith.reset();
        ab_.restless = false;
        abEnding_ = ending;
        if (ending == 0)
        {
            abEndingFiles_.clear();
            abEndingDials_.clear();
            const std::lock_guard<std::mutex> lock(programMutex_);
            abKept_ = {};
        }
        ++ab_.st.revision;
        abPublish();
    }

    void Processor::abPublish()
    {
        const AbState& st = ab_.st;
        // The trims: Close's or Keep's compile untrimmed, each side's at its gain (while it is on, or until that compile
        // lands); a program none of them names keeps the gain that plays meanwhile.
        const bool live = st.on || abEnding_ != 0;
        std::array<uint64_t, AbTrims::kPrograms> programs{};
        std::array<float, AbTrims::kPrograms> gains{};
        gains.fill(1.0f);
        programs[0] = abEnding_;
        for (std::size_t k = 0; k < 2; ++k)
            if (live && ab_.program[k] != abEnding_)
            {
                programs[k + 1] = ab_.program[k];
                gains[k + 1] = st.match ? std::pow(10.0f, std::min(0.0f, st.trimDb[k]) / 20.0f) : 1.0f;
            }
        const uint32_t sequence = abTrims_.sequence.load(std::memory_order_relaxed);
        abTrims_.sequence.store(sequence + 1u, std::memory_order_relaxed);   // odd: being written
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i = 0; i < AbTrims::kPrograms; ++i)
        {
            abTrims_.program[i].store(programs[i], std::memory_order_relaxed);
            abTrims_.gain[i].store(gains[i], std::memory_order_relaxed);
        }
        abTrims_.hold.store(live, std::memory_order_relaxed);
        abTrims_.sequence.store(sequence + 2u, std::memory_order_release);
        // The newest request it made, which a session loaded takes back while it waits (setStateInformation).
        abQueued_.store(std::max(st.on ? ab_.pendingRequest : uint64_t{ 0 }, abEnding_));
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
                line += "; measuring the loudness (at the dials as they are)";
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
    bool Processor::trimLookup(uint64_t program, float& gain, bool& known, bool& hold) const noexcept
    {
        const uint32_t sequence = abTrims_.sequence.load(std::memory_order_acquire);
        if ((sequence & 1u) != 0u)
            return false;                                        // being written
        gain = 1.0f;
        known = false;
        for (std::size_t i = 0; i < AbTrims::kPrograms; ++i)
            if (!known && program != 0 && abTrims_.program[i].load(std::memory_order_relaxed) == program)
            {
                gain = abTrims_.gain[i].load(std::memory_order_relaxed);
                known = true;
            }
        hold = abTrims_.hold.load(std::memory_order_relaxed) && !abLoading_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (abTrims_.sequence.load(std::memory_order_relaxed) != sequence)
            return false;
        if (!(gain >= 0.0f && gain <= 1.0f))
            gain = 1.0f;
        return true;
    }

    // A program offered that the A/B plays lower than the gain that plays now waits until the trim has come down to it
    // (a ramp of 20 ms: the program that plays turned down, never up), so it plays at its trim from its first sample.
    void Processor::trimGate() noexcept
    {
        const uint64_t next = engine_.pendingProgram();
        bool gate = false;
        if (next != 0)
        {
            float gain = 1.0f;
            bool known = false, hold = false;
            if (!trimLookup(next, gain, known, hold))
                gate = gating_;                                  // the table moved under the read: as it was
            else if (known && hold && trimSet_)
            {
                gateGain_ = gain;
                gate = trimTo_ > gain || trimNow_ > gain || trimFade_.moving();
            }
        }
        gating_ = gate;
        engine_.holdPending(gate);
    }

    // The Engine's output turned by the trim of the program that plays (one the A/B does not know: the gain it aims at
    // already while it is on or ending, else 1), the gain ramping over 20 ms when it changes; untouched while the gain
    // is 1 and still. The table is read inside its sequence count: moved under the read, the aim stays as it was. A
    // higher gain waits while the program going out still sounds (it played at the lower), and a lower one coming is
    // aimed at at once (trimGate holds that program meanwhile).
    void Processor::trimOutput(float* const* channels, int numChannels, int numSamples) noexcept
    {
        const uint64_t program = engine_.playingProgram();
        float target = trimTo_;
        float gain = 1.0f;
        bool known = false, hold = false;
        if (trimLookup(program, gain, known, hold))
            target = known ? gain : hold && program != 0 ? trimTo_ : 1.0f;
        if (trimSet_ && target > trimTo_ && engine_.outgoing())
            target = trimTo_;
        if (gating_ && gateGain_ < target)
            target = gateGain_;
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
