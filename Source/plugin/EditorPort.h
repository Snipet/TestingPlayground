// Source/plugin/EditorPort.h: what the editor reaches besides the parameter ports (JUCE-free; message thread only).
// The processor implements it (Processor.h); the Panel takes it as it takes the ParamPorts, so the editor never
// includes the processor.
//
//   draft()           the editor's files and the tab it shows, kept by the processor (and in the session) so they
//                     outlive the editor's window
//   draftRevision()   bumps when the draft changes other than by setDraft (a host loading a session), so an open editor
//                     shows the new files
//   setDraft()        the editor's files, as they are typed, and its tab
//   run()             RUN: compiles `files` off the message thread (Compiler.h) and plays them once they compile; the
//                     dials of parameters the program did not have before move to their defaults then
//   compiling()       a run is waiting or compiling
//   status()          the last compile's outcome: FMinus's messages, the dials, the time (CompileStatus)
//   statusRevision()  bumps with each compile that finishes
//   running()         the files of the program that plays (none before the first compiles)
//   controls()        the dials of the program that plays
//   panel()           its interface: a .gui file's (GuiSpec.h), or how its #pragma panel lines lay its dials out
//                     (PanelSpec.h)
//   panelView()       the editor shows the PANEL view (the interface across the left column) rather than the CODE view;
//                     kept in the session, set with setPanelView()
//   program()         the compile it came from (CompileStatus::request): the meters' taps are its fields once
//                     Meters::program says so (until the audio thread takes it, they are the last program's)
//   meters()          the last block's levels and the program's taps, for the panel's displays (Engine::Meters); false
//                     while the audio thread publishes (keep the last)
//   scope()           the last n samples of the input or the output (Engine::scope)
//   addMeterReader()  meters() and scope() are measured only while someone reads them (Engine::readers): a Panel adds
//   removeMeterReader() itself in its constructor and removes itself in its destructor; meterReaders() how many now
//   load()            the audio thread's busiest recent block, as a share of its time (Engine::load)
//   alerts()          the output guard's alerts of the last two seconds (Engine::Alert bits)
//   projects()        the projects saved on this Mac (ProjectStore.h), by name: each a program and its dials, kept as
//                     versions (every save adds one), with presets (dial settings for it)
//   projectFolder()   where they are, "~/..." for the home folder
//   tidyName()        a typed project's or preset's name as it would be saved ("" when nothing is left of it)
//   project()         the project open (the one opened or saved last; none at first), its version, the preset applied,
//                     and whether the editor's files and the dials have changed since; kept in the session
//   versions()        a project's versions, the newest first; presets() its presets, by name
//   saveVersion()     the editor's files (the draft) and the dials as they are now, by name, as a new version of the
//                     project `name` (a new project when there is none): it is then the project open. No new version
//                     when nothing changed since the latest (Saved::unchanged). False, with why, when it cannot
//   openProject()     a version's files (0: the latest) become the draft (the editor shows them: draftRevision bumps)
//                     and run; its dials are set by name once they compile
//   deleteProject()   its file deleted; deleteVersion() one version (never the only one)
//   savePreset()      the dials as they are now, by name, as a preset of the project open (over one of that name)
//   applyPreset()     a preset's dials, by name: at once when its project is open, else after its latest version is
//                     opened and compiled
//   deletePreset()    a preset deleted
//   projectInfo()     a project's description, author, category, tags and genres; setProjectInfo() keeps them (tidied)
//   exportProject()   a project written as a .zip of its source or a .fmpack (ProjectExport.h), every version or the
//                     newest only; exportFolder() where a chooser starts (the Desktop)
//   importProject()   a .zip or a .fmpack exported by this plugin, added as a new project (its name, or "<name> 2"); it is
//                     not opened
//   chat()            the chat with Claude (ChatPort.h)
#pragma once

#include <cctype>
#include <string_view>

#include "plugin/ChatPort.h"

#include "dsp/Compiler.h"
#include "dsp/Controls.h"
#include "dsp/Engine.h"
#include "dsp/Sources.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace funkgui
{
    class ParamPort;
}

namespace playground
{
    class EditorPort;

    // A picture of the editor, for the chat's view_panel tool: a Panel of its own over the processor's ports and its
    // EditorPort, laid out at the editor's size and drawn headless (FunkGui's SoftRaster: a shader as a placeholder),
    // as PNG bytes. `window`: the whole editor as the user has it; else the program's panel in the PANEL view, cropped
    // to it. `page`: a tabs element's page to show, by its title ("" for the user's). The view, the chat's fold and
    // the pages the user has stay as they were. Message thread only. Defined in Source/editor/Picture.cpp.
    struct PanelPicture
    {
        std::string png;                         // the PNG's bytes ("" when it could not be made: `why`)
        int width = 0, height = 0;               // its pixels
        std::string about;                       // what it shows (the view, its size, the pages there are)
        std::string why;
    };
    PanelPicture picturePanel(std::span<funkgui::ParamPort* const> ports, EditorPort& port, int width, int height,
                              bool window, const std::string& page);
    // The editor's files, a tab each (the first the program), and the tab it shows.
    struct Draft
    {
        Sources files;
        int active = 0;

        friend bool operator==(const Draft&, const Draft&) = default;
    };

    struct ProjectEntry
    {
        std::string name;
        std::string when;                        // its latest version's save, "2026-10-25 14:02" (local time)
        int latest = 0;                          // its latest version's number
        int versions = 0;                        // how many it keeps
        int presets = 0;
    };

    struct VersionEntry
    {
        int number = 0;
        std::string when;
        std::string note;                        // what changed since the version before ("program.fm +12 -3, 2 dials")
    };

    struct PresetEntry
    {
        std::string name;
        std::string when;
        int from = 0;                            // the version it was saved with
    };

    struct ProjectState
    {
        std::string name;                        // "" for none
        int version = 0;                         // the version opened or saved last (0: none kept)
        int latest = 0;                          // the project's newest
        std::string preset;                      // the preset applied last ("" for none)
        bool edited = false;                     // the editor's files differ from that version's
        bool dialsEdited = false;                // a dial moved from the preset's (or the version's) value
    };

    struct ProjectInfo
    {
        std::string description;
        std::string author;
        std::string category;
        std::vector<std::string> tags;
        std::vector<std::string> genres;
        friend bool operator==(const ProjectInfo&, const ProjectInfo&) = default;
    };

    // A file's name a project export's (ProjectExport.h): what Import offers and a drop on the panel imports.
    inline bool isProjectFile(std::string_view path)
    {
        const std::size_t dot = path.rfind('.');
        if (dot == std::string_view::npos)
            return false;
        std::string ext(path.substr(dot + 1));
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return ext == "fmpack" || ext == "zip";
    }

    struct Exported
    {
        int64_t bytes = 0;                       // the file
        int64_t raw = 0;                         // what it holds, uncompressed
        int versions = 0;
        std::string method;                      // "xz", "zlib" or "stored" (a package's), "zip"
    };

    struct SavedVersion
    {
        int number = 0;
        bool unchanged = false;                  // nothing changed since the latest: no version added
        bool created = false;                    // a new project
    };

    class EditorPort
    {
    public:
        virtual ~EditorPort() = default;

        virtual Draft draft() const = 0;
        virtual uint64_t draftRevision() const = 0;
        virtual void setDraft(const Draft& draft) = 0;

        virtual void run(const Sources& files) = 0;
        virtual bool compiling() const = 0;
        virtual CompileStatus status() const = 0;
        virtual uint64_t statusRevision() const = 0;
        virtual Sources running() const = 0;
        virtual Controls controls() const = 0;
        virtual PanelSpec panel() const = 0;
        virtual uint64_t program() const = 0;
        virtual bool meters(Meters& out) const = 0;
        virtual void scope(bool input, float* out, int n) const = 0;
        virtual void addMeterReader() = 0;
        virtual void removeMeterReader() = 0;
        virtual int meterReaders() const = 0;

        virtual float load() const = 0;
        virtual uint32_t alerts() const = 0;
        virtual bool frozen() const = 0;                 // the dials are frozen into the code (Patch.h)

        virtual std::vector<ProjectEntry> projects() const = 0;
        virtual std::string projectFolder() const = 0;
        virtual std::string tidyName(const std::string& typed) const = 0;
        virtual ProjectState project() const = 0;
        virtual std::vector<VersionEntry> versions(const std::string& project) const = 0;
        virtual std::vector<PresetEntry> presets(const std::string& project) const = 0;
        virtual bool saveVersion(const std::string& name, SavedVersion& saved, std::string& why) = 0;
        virtual bool openProject(const std::string& name, int version, std::string& why) = 0;
        virtual bool deleteProject(const std::string& name, std::string& why) = 0;
        virtual bool deleteVersion(const std::string& name, int version, std::string& why) = 0;
        virtual bool savePreset(const std::string& preset, std::string& why) = 0;
        virtual bool applyPreset(const std::string& project, const std::string& preset, std::string& why) = 0;
        virtual bool deletePreset(const std::string& project, const std::string& preset, std::string& why) = 0;
        virtual ProjectInfo projectInfo(const std::string& project) const = 0;
        virtual bool setProjectInfo(const std::string& project, const ProjectInfo& info, std::string& why) = 0;
        virtual bool exportProject(const std::string& project, bool package, bool allVersions, const std::string& path,
                                   Exported& out, std::string& why) = 0;
        virtual bool importProject(const std::string& path, std::string& name, std::string& why) = 0;
        virtual std::string exportFolder() const = 0;
        // Claude Desktop (plugin/McpLink.h): this instance's name to it ("Synth 2"; "" when there is no link), and
        // whether Claude Desktop is talking to it now.
        virtual std::string desktopName() const = 0;
        virtual bool desktopConnected() const = 0;
        virtual ChatPort& chat() = 0;

        virtual bool panelView() const = 0;
        virtual void setPanelView(bool on) = 0;

        // Whether the chat's column is open (else folded into a rail at the right), kept in the session.
        virtual bool chatOpen() const = 0;
        virtual void setChatOpen(bool open) = 0;
        // Whether the explorer (the program's files, the library, the examples and the presets: editor/Explorer.h) is
        // open at the left (else folded into a rail), kept in the session.
        virtual bool explorerOpen() const = 0;
        virtual void setExplorerOpen(bool open) = 0;

        // The pages the panel's interface shows: each tabs element's chosen page (the tabs in the order the .gui has
        // them), by its title. Kept in the session, so a reopened editor and a reloaded session show them again.
        virtual std::vector<std::string> guiPages() const = 0;
        virtual void setGuiPages(const std::vector<std::string>& titles) = 0;

        // A note from the panel's keyboard (a .gui file's keyboard element): `velocity` 0..1 plays it, 0 releases it.
        // An instrument plays it at the start of its next block; an effect ignores it. 64 can wait at once; past that
        // a note is dropped.
        virtual void playNote(int note, float velocity) = 0;
        // The keyboard's wheels: the pitch wheel (`pitch`, -1..1) or the mod wheel (0..1), as MIDI's would be.
        virtual void playWheel(bool pitch, float value) = 0;
    };
}
