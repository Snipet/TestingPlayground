// Source/plugin/ProcessorSaves.cpp: Claude's projects tool on the processor's side (ChatHost::chatProjects; the tool is
// ChatProjects.cpp). It acts as the editor's PROJECTS does, through the same calls (saveVersionAs, savePresetAs,
// openProjectAs, applyPresetAs), on the message thread: the caller's worker (the chat's turn, or the MCP link's) hands
// poll() a job and waits for it. A list reads the store on the caller's thread instead, as PROJECTS' own listing may
// (a project's file is never read whole on the message thread for it), and the project open's state from a job.
//
// Safe by design (ChatAgent.h's ProjectCall): a save only adds (a version, a preset, the user's own copy of a factory
// project: the factory's is never written); a preset of the name is replaced only with `replace`, and the result gives
// its old values; opening a project or applying a preset while the editor holds unsaved changes (its files changed
// since they were opened, saved or loaded; a dial moved from the version's or the preset's value, or from its default
// when no project is open) is refused unless `discard`; a save or a preset while an A/B plays is refused (what plays is
// a side's, not the editor's); nothing is deleted. Each save, open and preset goes to the editor's status (claudeNotice:
// "CLAUDE SAVED VERSION 5 OF ENGINE", which the status line says in its sentence case, as it says every notice), and,
// the plugin's own chat's, to the journal (get_status since): Claude Desktop's are not journalled (Journal.h: it knows
// what it did).
#include "plugin/Processor.h"

#include "Product.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace playground
{
    namespace
    {
        std::string inQuotes(const std::string& s) { return "\"" + s + "\""; }

        std::string upper(std::string s)
        {
            for (char& c : s)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return s;
        }

        std::string dateOf(int64_t ms)
        {
            return juce::Time(static_cast<juce::int64>(ms)).formatted("%Y-%m-%d %H:%M").toStdString();
        }

        std::string plainNumber(float v)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.4g", static_cast<double>(v));
            return buf;
        }

        // A version or a preset's dial as the panel says it (its unit, its option's name), when the program has it.
        std::string dialWords(const projectstore::Dial& d, const Controls& cs,
                              const std::array<std::vector<std::string>, kMaxControls>& options)
        {
            for (int c = 0; c < kMaxControls; ++c)
                if (const Control& k = cs[static_cast<std::size_t>(c)]; k.used && d.name == k.name)
                    return dialText(k, projectstore::valueOf(d, k, options[static_cast<std::size_t>(c)]),
                                    options[static_cast<std::size_t>(c)]);
            return d.label.empty() ? plainNumber(d.value) : d.label;
        }

        constexpr std::size_t kMaxDialsSaid = 12;       // a result's dials, then "and n more"
        constexpr std::size_t kMaxPresetsSaid = 16;     // a project's presets in the list of every project, then the same

        std::string dialList(const std::vector<projectstore::Dial>& dials, const Controls& cs,
                             const std::array<std::vector<std::string>, kMaxControls>& options)
        {
            std::string out;
            for (std::size_t i = 0; i < dials.size() && i < kMaxDialsSaid; ++i)
                out += (i == 0 ? "" : ", ") + dials[i].name + " " + dialWords(dials[i], cs, options);
            if (dials.size() > kMaxDialsSaid)
                out += ", and " + std::to_string(dials.size() - kMaxDialsSaid) + " more";
            return out.empty() ? std::string("no dials") : out;
        }

        // open's name: "Name", "Name@N" (a version), "@N" (the project open's version N); the version 0 for the newest.
        // A project whose own name ends so is taken whole.
        std::pair<std::string, int> nameAt(const std::string& name)
        {
            const std::size_t at = name.rfind('@');
            if (at == std::string::npos || at + 1 >= name.size() || projectstore::exists(name))
                return { name, 0 };
            const std::string n = name.substr(at + 1);
            if (!std::all_of(n.begin(), n.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })
                || n.size() > 6)
                return { name, 0 };
            return { name.substr(0, at), std::stoi(n) };
        }
    }

    // ---- the message thread's hand -------------------------------------------------------------------------------------
    bool Processor::onMessageThread(std::function<void()> job)
    {
        if (juce::MessageManager::existsAndIsCurrentThread())
        {
            job();
            return true;
        }
        std::unique_lock<std::mutex> lock(claudeJobMutex_);
        claudeJobDone_.wait(lock, [this] { return !claudeJob_ && !claudeJobRunning_; });   // one at a time
        claudeJob_ = std::move(job);
        const uint64_t ticket = claudeJobsDone_ + 1;
        if (!claudeJobDone_.wait_for(lock, std::chrono::milliseconds(claudeJobMs_),
                                     [&] { return claudeJobRunning_ || claudeJobsDone_ >= ticket; }))
        {
            claudeJob_ = nullptr;                                // never taken: withdrawn, nothing done
            claudeJobDone_.notify_all();
            return false;
        }
        claudeJobDone_.wait(lock, [&] { return claudeJobsDone_ >= ticket; });   // taken: it finishes
        return true;
    }

    bool Processor::claudeJobWaiting() const
    {
        const std::lock_guard<std::mutex> lock(claudeJobMutex_);
        return static_cast<bool>(claudeJob_);
    }

    void Processor::runClaudeJob()
    {
        std::function<void()> job;
        {
            const std::lock_guard<std::mutex> lock(claudeJobMutex_);
            if (!claudeJob_)
                return;
            job = std::move(claudeJob_);
            claudeJob_ = nullptr;
            claudeJobRunning_ = true;
        }
        claudeJobDone_.notify_all();
        job();
        {
            const std::lock_guard<std::mutex> lock(claudeJobMutex_);
            claudeJobRunning_ = false;
            ++claudeJobsDone_;
        }
        claudeJobDone_.notify_all();
    }

    void Processor::noticeClaude(const std::string& text)
    {
        const std::lock_guard<std::mutex> lock(claudeNoticeMutex_);
        ++claudeNotice_.count;
        claudeNotice_.text = text;
    }

    ClaudeNotice Processor::claudeNotice() const
    {
        const std::lock_guard<std::mutex> lock(claudeNoticeMutex_);
        return claudeNotice_;
    }

    // ---- the tool --------------------------------------------------------------------------------------------------------
    ProjectAnswer Processor::chatProjects(const ProjectCall& call)
    {
        if (call.action == ProjectCall::Action::list)
            return { true, claudeProjectList(call), call.name.empty() ? "looked at the projects" : "looked at " + inQuotes(call.name), false };
        ProjectAnswer out;
        uint64_t compile = 0;
        if (!onMessageThread([&] { out = claudeProjectAction(call, compile); }))
            return { false, "The plugin did not get to it in time (its host may be busy): nothing was done. Try again.", {}, false };
        if (out.ok && compile != 0)
        {
            // An open: its compile, then a poll() (a job of its own) that has set its dials, so what the next tool
            // measures is the version as it was saved. A poll() under way as the compile ended may take the job before
            // it has seen the compile: asked again until one has (dialsRequest_ cleared), a few times at most.
            compiler_.wait();
            const CompileStatus s = compiler_.status();
            if (s.request >= compile && s.ok)
                out.text += "\nIt compiled and plays.";
            else if (s.request >= compile)
            {
                const std::string first = s.errors.substr(0, s.errors.find('\n'));
                out.text += "\nIt did not compile, so the program before it plays on (" + first
                          + "): run_program gives every message once its files are fixed.";
            }
            if (!juce::MessageManager::existsAndIsCurrentThread())
                for (int k = 0; k < 5; ++k)
                {
                    bool landed = false;
                    if (!onMessageThread([&] { landed = dialsRequest_ != compile; }) || landed)
                        break;
                }
        }
        return out;
    }

    // The message thread's part: one action as PROJECTS does it, and the answer.
    ProjectAnswer Processor::claudeProjectAction(const ProjectCall& call, uint64_t& compile)
    {
        using A = ProjectCall::Action;
        const auto fail = [](std::string why) { return ProjectAnswer{ false, std::move(why), {}, false }; };
        const std::optional<Journal::Who> who =
            call.desktop ? std::nullopt : std::optional<Journal::Who>(Journal::Who::chat);
        // An A/B plays a side's files and dials while the editor keeps its own (ProcessorAb.cpp), and until Keep's or
        // Close's compile lands what plays is still a side's: a save would keep the editor's files with a side's dials,
        // and a preset would move dials the A/B puts back. The user cannot save then either (the A/B card is modal);
        // an open ends it, as any program that lands does.
        if (call.action != A::open && call.action != A::list && (ab_.st.on || abEnding_ != 0))
        {
            const std::string none = call.action == A::applyPreset ? "Not applied: " : "Nothing was saved: ";
            return fail(ab_.st.on ? none + "the user is comparing A and B (A/B), so what plays is a side, not the editor's "
                                           "files and dials. Ask them to Keep a side or Close it first."
                                  : none + "the A/B the user just ended is still putting back what played before. Try "
                                           "again in a moment.");
        }
        const ProjectState st = project();
        Controls cs;
        std::array<std::vector<std::string>, kMaxControls> options;
        std::vector<projectstore::Dial> base;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            cs = controls_;
            options = panel_.options;
            base = baseDials_;
        }
        // What the editor holds that no version or preset has: the files changed since they were opened, saved or loaded,
        // the dials moved from what the version or the preset set (none open: from their defaults).
        const auto unsaved = [&](bool files) {
            std::vector<std::string> moved;
            if (dialsRequest_ == 0)                              // (a version's dials on their way: none yet)
                for (int c = 0; c < kMaxControls; ++c)
                {
                    const Control& k = cs[static_cast<std::size_t>(c)];
                    if (!k.used)
                        continue;
                    float saved = k.def;
                    if (!st.name.empty())
                    {
                        const auto it = std::find_if(base.begin(), base.end(), [&](const projectstore::Dial& d) { return d.name == k.name; });
                        if (it == base.end())
                            continue;
                        saved = projectstore::valueOf(*it, k, options[static_cast<std::size_t>(c)]);
                    }
                    const float norm = raw_[static_cast<std::size_t>(c)]->load();
                    const float plain = std::isfinite(norm) ? plainOf(k, norm) : k.def;
                    if (std::fabs(plain - saved) > 1.0e-4f * std::max(1.0e-6f, std::fabs(k.max - k.min)))
                        moved.push_back(k.name);
                }
            std::string words;
            if (files && st.changed)
                words = st.name.empty() ? "the editor's files (no project holds them)"
                                        : "the editor's files (changed since version " + std::to_string(st.version) + ")";
            if (!moved.empty())
            {
                std::string names;
                for (std::size_t i = 0; i < moved.size() && i < 4; ++i)
                    names += (i == 0 ? "" : ", ") + moved[i];
                if (moved.size() > 4)
                    names += ", ...";
                words += (words.empty() ? "" : " and ") + std::to_string(moved.size()) + (moved.size() == 1 ? " dial" : " dials")
                       + " moved (" + names + ") from "
                       + (st.name.empty()        ? std::string("their defaults")
                          : !st.preset.empty()   ? "the preset " + inQuotes(st.preset)
                                                 : "version " + std::to_string(st.version));
            }
            return words;
        };
        std::string why;
        switch (call.action)
        {
            case A::list:
                break;
            case A::saveVersion:
            {
                const std::string name = call.name.empty() ? st.name : call.name;
                if (name.empty())
                    return fail("No project is open: name the new project (name). Nothing was saved.");
                const bool another = !st.name.empty() && !isFactoryName(name) && !projectstore::sameName(projectstore::tidyName(name), st.name)
                                  && projectstore::exists(name);
                const std::string note = projectstore::noteLead(call.note);
                SavedVersion saved;
                if (!saveVersionAs(name, note.empty() ? std::string("Claude") : "Claude: " + note, who, saved, why))
                    return fail("Nothing was saved: " + why + ".");
                const std::string into = saved.name.empty() ? projectstore::tidyName(name) : saved.name;
                std::string text;
                if (saved.unchanged)
                    text = "Nothing new to save: version " + std::to_string(saved.number) + " of " + inQuotes(into)
                         + " has these files and dials already, so no version was added.";
                else if (saved.copied)
                    text = "Saved " + inQuotes(into) + ", the user's own copy of the factory's " + inQuotes(ownName(st.name.empty() || !isFactoryName(name) ? st.name : name))
                         + " (which stays as it is): its version 1, with the factory project's presets. It is the project open now.";
                else if (saved.created)
                    text = "Saved " + inQuotes(into) + ", a new project: its version 1. It is the project open now.";
                else
                    text = "Saved version " + std::to_string(saved.number) + " of " + inQuotes(into)
                         + (another ? " (which was not the project open: it is now)" : "")
                         + ". Every version is kept: the user opens any in PROJECTS.";
                if (!note.empty() && !saved.unchanged)
                    text += " Its note: Claude: " + note + ".";
                // The category, whether or not a version was added (an unchanged project's is set all the same: it is
                // the only way Claude sets one), and said either way.
                std::string category;
                if (!call.category.empty())
                {
                    const std::string was = projectInfo(into).category, now = projectstore::tidyCategory(call.category);
                    if (now.empty())
                        text += " Its category stays " + (was.empty() ? std::string("unset") : was) + ": the category given names none.";
                    else if (projectstore::sameName(was, now))
                        text += " Its category is " + was + " already.";
                    else if (projectstore::setCategory(into, now, &why))
                    {
                        text += " Its category: " + now + (was.empty() ? "" : " (was " + was + ")") + ".";
                        category = now;
                    }
                    else
                        text += " Its category was not set: " + why + ".";
                }
                if (saved.unchanged)
                {
                    if (category.empty())
                        return { true, text, "nothing new to save in " + inQuotes(into), false };
                    noticeClaude("CLAUDE PUT " + upper(into) + " IN " + upper(category));
                    return { true, text, "put " + inQuotes(into) + " in " + category + " (nothing new to save)", false };
                }
                const std::string at = saved.copied || saved.created ? inQuotes(into) : "version " + std::to_string(saved.number) + " of " + inQuotes(into);
                noticeClaude(saved.copied    ? "CLAUDE SAVED " + upper(into) + ", YOUR OWN COPY (THE FACTORY'S STAYS AS IT IS)"
                             : saved.created ? "CLAUDE SAVED " + upper(into) + " (A NEW PROJECT)"
                                             : "CLAUDE SAVED VERSION " + std::to_string(saved.number) + " OF " + upper(into));
                return { true, text, "saved " + at, false };
            }
            case A::savePreset:
            {
                if (st.name.empty())
                    return fail("No project is open, and a preset is kept in its project: save the editor's files as a project "
                                "first (save_version with a name), if the user wants one. Nothing was saved.");
                if (isFactoryName(st.name))
                    return fail(inQuotes(ownName(st.name)) + " is a factory project, read-only: save_version first makes the user's own "
                                "copy (its presets with it), then save_preset saves into it. Nothing was saved.");
                const std::string name = projectstore::tidyName(call.name);
                if (name.empty())
                    return fail("A preset needs a name (name). Nothing was saved.");
                std::optional<projectstore::PresetSummary> old;
                if (const auto ps = projectstore::presetsOf(st.name, &why))
                    for (const projectstore::PresetSummary& p : *ps)
                        if (projectstore::sameName(p.name, name))
                            old = p;
                if (old && !call.replace)
                    return fail(inQuotes(st.name) + " has a preset " + inQuotes(old->name) + " already (saved " + dateOf(old->savedMs)
                                + "): nothing was saved. Choose another name, or pass replace: true if the user asked to replace it.");
                const std::vector<projectstore::Dial> now = dialsNow();
                if (!savePresetAs(name, who, why))
                    return fail("Nothing was saved: " + why + ".");
                std::string text = (old ? "Replaced the preset " : "Saved the preset ") + inQuotes(old ? old->name : name) + " of "
                                 + inQuotes(st.name) + ": " + dialList(now, cs, options) + ". It is the preset applied now.";
                if (old)
                {
                    std::string changed;
                    int same = 0;
                    for (const projectstore::Dial& d : old->dials)
                    {
                        const auto it = std::find_if(now.begin(), now.end(), [&](const projectstore::Dial& n) { return n.name == d.name; });
                        const std::string was = dialWords(d, cs, options);
                        if (it != now.end() && dialWords(*it, cs, options) == was)
                        {
                            ++same;
                            continue;
                        }
                        changed += (changed.empty() ? "" : ", ") + d.name + " " + was
                                 + (it == now.end() ? " (not in the program now)" : " (now " + dialWords(*it, cs, options) + ")");
                    }
                    text += " Its old values: " + (changed.empty() ? std::string("the same") : changed)
                          + (same > 0 && !changed.empty() ? "; " + std::to_string(same) + " more the same" : "") + ".";
                }
                // A category that tidies to nothing is none given: a preset replaced keeps its own.
                if (const std::string cat = projectstore::tidyCategory(call.category); !cat.empty())
                {
                    if (projectstore::setPresetCategory(st.name, name, cat, &why))
                    {
                        text += " Its category: " + cat + (old && !old->category.empty() && !projectstore::sameName(old->category, cat)
                                                               ? " (was " + old->category + ")"
                                                               : "") + ".";
                        refreshPresets(true);
                    }
                    else
                        text += " Its category was not set: " + why + ".";
                }
                else if (old && !old->category.empty())
                    text += " Its category stays " + old->category + ".";
                if (st.edited)
                    text += " The editor's files differ from version " + std::to_string(st.version)
                          + ", which the preset names as its own: save_version keeps them, if the user wants.";
                noticeClaude((old ? "CLAUDE REPLACED PRESET " : "CLAUDE SAVED PRESET ") + upper(name) + " IN " + upper(st.name));
                return { true, text, (old ? "replaced preset " : "saved preset ") + inQuotes(name) + " of " + inQuotes(st.name), false };
            }
            case A::applyPreset:
            {
                if (st.name.empty())
                    return fail("No project is open: its presets are a project's. Open one first (open), if the user asked.");
                if (call.name.empty())
                    return fail("Name the preset (name).");
                std::optional<projectstore::PresetSummary> pr;
                std::string have;
                if (const auto ps = projectstore::presetsOf(st.name, &why))
                    for (const projectstore::PresetSummary& p : *ps)
                    {
                        have += (have.empty() ? "" : ", ") + p.name;
                        if (projectstore::sameName(p.name, call.name))
                            pr = p;
                    }
                if (!pr)
                    return fail(inQuotes(st.name) + " has no preset " + inQuotes(call.name) + ". Its presets: "
                                + (have.empty() ? std::string("none") : have) + ".");
                const std::string lost = unsaved(false);
                if (!lost.empty() && !call.discard)
                    return fail("Not applied: the editor holds unsaved changes (" + lost + "), which applying " + inQuotes(pr->name)
                                + " would lose. Save them first (save_preset or save_version) if the user wants them; pass "
                                  "discard: true only if the user said to drop them.");
                if (!applyPresetAs(st.name, pr->name, who, why))
                    return fail("Not applied: " + why + ".");
                std::string text = "Applied the preset " + inQuotes(pr->name) + " of " + inQuotes(st.name)
                                 + ": the dials glide to its values (" + dialList(pr->dials, cs, options) + ").";
                if (!lost.empty())
                    text += " What the editor held is dropped, as the user asked: " + lost + ".";
                noticeClaude("CLAUDE APPLIED PRESET " + upper(pr->name));
                return { true, text, "applied preset " + inQuotes(pr->name), false };
            }
            case A::open:
            {
                auto [name, version] = nameAt(call.name);
                if (name.empty())
                    name = st.name;
                if (name.empty())
                    return fail("Name the project to open (name; Name@N for its version N). list names them.");
                const std::optional<projectstore::Project> p = projectstore::load(name, &why);
                if (!p)
                    return fail("Not opened: " + why + ". list names the projects.");
                const projectstore::Version* v = version <= 0 ? &p->latest() : p->version(version);
                if (v == nullptr)
                {
                    std::string kept;
                    for (const projectstore::Version& k : p->versions)
                        kept += (kept.empty() ? "" : ", ") + std::to_string(k.number);
                    return fail(inQuotes(p->name) + " keeps no version " + std::to_string(version) + ". It keeps " + kept + ".");
                }
                const std::string lost = unsaved(true);
                if (!lost.empty() && !call.discard)
                    return fail("Not opened: the editor holds unsaved changes (" + lost + "), which opening " + inQuotes(p->name)
                                + " would lose. Save them first (save_version) if the user wants them; pass discard: true only "
                                  "if the user said to drop them.");
                if (!openProjectAs(p->name, v->number, who, why))
                    return fail("Not opened: " + why + ".");
                compile = dialsRequest_;
                const bool factory = isFactoryName(p->name);
                std::string text = "Opened version " + std::to_string(v->number) + " of " + inQuotes(p->name) + " (of "
                                 + std::to_string(p->latest().number) + "; saved " + dateOf(v->savedMs) + ": " + v->note
                                 + "): its files are the editor's now (one step the user can undo there) and run, and its dials "
                                   "follow once it compiles.";
                if (factory)
                    text += " It is a factory project, read-only: save_version saves the user's own copy.";
                if (!lost.empty())
                    text += " What the editor held is dropped, as the user asked: " + lost + ".";
                noticeClaude("CLAUDE OPENED " + upper(ownName(p->name)) + " V" + std::to_string(v->number));
                // Another project's program (or the first project over the editor's own files): Claude's checks were the
                // one before's (the agent clears them, as a fresh start does).
                const bool another = st.name.empty() || !projectstore::sameName(st.name, p->name);
                return { true, text, "opened version " + std::to_string(v->number) + " of " + inQuotes(p->name), another };
            }
        }
        return fail("Nothing was done.");
    }

    // The caller's part of a list: the project open's state from the message thread, the store read here.
    std::string Processor::claudeProjectList(const ProjectCall& call)
    {
        ProjectState st;
        const bool known = onMessageThread([&] { st = project(); });
        std::string out;
        if (known)
            out = st.name.empty() ? std::string("No project is open")
                                  : "Open: " + inQuotes(st.name) + ", version " + std::to_string(st.version) + " of "
                                        + std::to_string(st.latest)
                                        + (st.preset.empty() ? std::string() : ", the preset " + inQuotes(st.preset));
        if (known)
        {
            std::string unsaved;
            if (st.changed)
                unsaved = st.name.empty() ? "the editor's files are in no project" : "the editor's files changed since";
            if (st.dialsEdited)
                unsaved += (unsaved.empty() ? "" : "; ") + std::string("dials moved since");
            out += unsaved.empty() ? std::string(st.name.empty() ? "." : "; nothing unsaved.") : " (unsaved: " + unsaved + ").";
            out += "\n";
        }
        std::string why;
        if (!call.name.empty())
        {
            const std::optional<projectstore::Project> p = projectstore::load(call.name, &why);
            if (!p)
                return out + "No project " + inQuotes(call.name) + ": " + why + ".";
            out += inQuotes(p->name) + (isFactoryName(p->name) ? " (a factory project, read-only)" : std::string())
                 + (p->info.category.empty() ? std::string() : ", category " + p->info.category)
                 + (p->info.description.empty() ? std::string() : ": " + p->info.description) + "\n";
            out += "Versions, the newest first:\n";
            for (auto it = p->versions.rbegin(); it != p->versions.rend(); ++it)
                out += "- " + std::to_string(it->number) + " (" + dateOf(it->savedMs) + "): " + it->note + "\n";
            out += p->presets.empty() ? std::string("No presets.\n") : "Presets:\n";
            for (const projectstore::Preset& pr : p->presets)
                out += "- " + pr.name + (pr.category.empty() ? std::string() : " [" + pr.category + "]")
                     + (pr.from > 0 ? ", saved with version " + std::to_string(pr.from) : std::string()) + "\n";
            return out;
        }
        const auto lines = [](const std::vector<projectstore::Entry>& list) {
            std::string s;
            for (const projectstore::Entry& e : list)
            {
                std::string presets;
                for (std::size_t i = 0; i < e.presets.size() && i < kMaxPresetsSaid; ++i)
                    presets += (i == 0 ? "" : ", ") + e.presets[i];
                if (e.presets.size() > kMaxPresetsSaid)
                    presets += ", and " + std::to_string(e.presets.size() - kMaxPresetsSaid) + " more (list with its name gives them)";
                s += "- " + e.name + (e.category.empty() ? std::string() : " [" + e.category + "]") + ": version "
                   + std::to_string(e.latest) + (e.versions > 1 ? " (" + std::to_string(e.versions) + " kept)" : std::string())
                   + "; " + (presets.empty() ? std::string("no presets") : "presets " + presets) + "\n";
            }
            return s;
        };
        const std::vector<projectstore::Entry> mine = projectstore::list(), factory = projectstore::factoryList();
        out += mine.empty() ? "The user has no projects on " + std::string(kThisMachine) + " yet.\n"
                            : "The user's projects (" + std::to_string(mine.size()) + "), [category]:\n" + lines(mine);
        if (!factory.empty())
            out += "Factory projects (read-only: a save makes the user's own copy):\n" + lines(factory);
        return out;
    }
}
