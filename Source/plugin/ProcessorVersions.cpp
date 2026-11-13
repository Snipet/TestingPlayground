// Source/plugin/ProcessorVersions.cpp: one version's content for the editor (EditorPort::versionPolled, editorContent;
// the Compare card, editor/DiffCard.h, and the A/B card's sides). A version comes from the store's table of the versions
// asked last (ProjectStore.h's versionCached): the message thread never reads a project's file whole, so one the table
// lacks goes to the store's Reader and `read` is false until projectsRead() moves. The dials are said as the panel says
// them, by the program that plays now (a dial it lacks: its value's name, else its number).
#include "plugin/Processor.h"

#include <cmath>
#include <cstdio>

namespace playground
{
    namespace
    {
        std::string whenText(int64_t ms) { return juce::Time(static_cast<juce::int64>(ms)).formatted("%Y-%m-%d %H:%M").toStdString(); }

        std::string plainText(float v)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
            return buf;
        }

        // Each dial with its text: the panel's words for it where the program that plays has a dial of its name.
        std::vector<ContentDial> dialTexts(const std::vector<projectstore::Dial>& dials, const Controls& cs,
                                           const std::array<std::vector<std::string>, kMaxControls>& options)
        {
            std::vector<ContentDial> out;
            for (const projectstore::Dial& d : dials)
            {
                ContentDial c{ d.name, d.value, d.label, {} };
                for (int k = 0; k < kMaxControls; ++k)
                    if (const Control& ctl = cs[static_cast<std::size_t>(k)]; ctl.used && ctl.name == d.name)
                    {
                        const auto& opts = options[static_cast<std::size_t>(k)];
                        c.text = dialText(ctl, projectstore::valueOf(d, ctl, opts), opts);
                        break;
                    }
                if (c.text.empty())
                    c.text = !d.label.empty() ? d.label : plainText(d.value);
                out.push_back(std::move(c));
            }
            return out;
        }
    }

    VersionContent Processor::versionPolled(const std::string& project, int number) const
    {
        VersionContent out;
        std::vector<projectstore::VersionAsk> unread;
        const std::optional<projectstore::VersionRead> r = projectstore::versionCached(project, number, unread);
        if (!unread.empty())
            reader_->requestVersions(unread);
        if (!r)
        {
            out.read = out.missing = true;               // no such project
            out.number = number;
            return out;
        }
        out.read = r->read;
        out.missing = r->missing;
        out.number = r->content.number;
        if (!r->read || r->missing)
            return out;
        Controls cs;
        std::array<std::vector<std::string>, kMaxControls> options;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            cs = controls_;
            options = panel_.options;
        }
        out.files = r->content.files;
        out.dials = dialTexts(r->content.dials, cs, options);
        for (const projectstore::SampleRef& s : r->content.samples)
            out.samples.push_back({ s.name, s.hash });
        out.note = r->content.note;
        out.when = whenText(r->content.savedMs);
        return out;
    }

    VersionContent Processor::editorContent() const
    {
        VersionContent out;
        out.read = out.editor = true;
        Controls cs;
        std::array<std::vector<std::string>, kMaxControls> options;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            out.files = draft_.files;
            cs = controls_;
            options = panel_.options;
        }
        out.dials = dialTexts(dialsNow(), cs, options);
        const Assets samples = assetsNow();
        for (const projectstore::SampleRef& s : projectstore::refsOf(samples.get()))
            out.samples.push_back({ s.name, s.hash });
        out.note = "the editor's files and dials as they are now";
        return out;
    }

    // Their side of a diverged project, as the git job read it (ProcessorGit.cpp): in memory, no file read here.
    VersionContent Processor::gitTheirs(const std::string& project) const
    {
        VersionContent out;
        out.read = true;
        std::optional<projectstore::Version> v;
        std::string sha;
        {
            const std::lock_guard<std::mutex> lock(gitMutex_);
            if (projectstore::sameName(gitTheirsOf_, project))
            {
                v = gitTheirsVersion_;
                sha = gitTheirsSha_;
            }
        }
        if (!v)
        {
            out.missing = true;
            return out;
        }
        Controls cs;
        std::array<std::vector<std::string>, kMaxControls> options;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            cs = controls_;
            options = panel_.options;
        }
        out.files = v->files;
        out.dials = dialTexts(v->dials, cs, options);
        for (const projectstore::SampleRef& s : v->samples)
            out.samples.push_back({ s.name, s.hash });
        out.note = "their side, " + sha.substr(0, 7) + ": " + v->note;
        out.when = whenText(v->savedMs);
        out.number = v->number;
        return out;
    }
}
