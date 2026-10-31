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
//   check()           the editor's check while typing: Patch::check of `files` on the compiler's thread
//                     (Compiler::check), nothing played; checkStatus() its outcome (CheckStatus::revision bumps)
//   running()         the files of the program that plays (none before the first compiles)
//   addIrViewer()     the IR view (the editor's IR tab, Compiler.h's "The IR"): the program's LLVM IR as text, kept only
//   removeIrViewer()  while someone looks: a Panel adds itself while its IR tab is open and removes itself when it
//                     closes (or the Panel goes); ir() what there is, irRevision() bumps when it changes. The IR is never
//                     one of the program's files: not in the draft, the session, a project or an export
//   controls()        the dials of the program that plays
//   panel()           its interface: a .gui file's (GuiSpec.h), or how its #pragma panel lines lay its dials out
//                     (PanelSpec.h)
//   fileViews()       each netlist's view (Netlist or Schematic), zoom and pan, as the editor's user left them; kept in
//                     the session, set with setFileViews() on the user's gestures only
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
//   windowButtons()   the Standalone's own window: its buttons over the top bar, their place, its drag (below)
//   devices()         the Standalone's audio and MIDI devices (DevicePort.h), for the settings card; null in a host
//   settingsAsked()   bumps each time the Standalone asks for the settings card (its menu's Settings..., Cmd-,)
#pragma once

#include <cctype>
#include <string_view>

#include "plugin/ChatPort.h"
#include "plugin/DevicePort.h"

#include "dsp/Compiler.h"
#include "dsp/Controls.h"
#include "dsp/Engine.h"
#include "dsp/Sources.h"

#include <cstddef>
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
    // EditorPort, laid out at the editor's size and drawn headless on the GPU, shaders as the screen draws them
    // (editor/Picture.h; where the GPU cannot, with SoftRaster, a shader as a striped placeholder, and `about` says so
    // and why), as PNG bytes. `window`: the whole editor as the user has it; else the program's panel in the PANEL
    // view, cropped to it. `page`: a tabs element's page to show, by its title ("" for the user's). The view, the chat's
    // fold and the pages the user has stay as they were. Message thread only. Defined in Source/editor/Picture.cpp.
    struct PanelPicture
    {
        std::string png;                         // the PNG's bytes ("" when it could not be made: `why`)
        int width = 0, height = 0;               // its pixels
        std::string about;                       // what it shows (the view, its size, the pages there are, the shaders)
        std::string why;
        bool gpu = false;                        // drawn by the GPU; false: SoftRaster (`about` says why)
    };
    PanelPicture picturePanel(std::span<funkgui::ParamPort* const> ports, EditorPort& port, int width, int height,
                              bool window, const std::string& page);

    // The pictures' GPU context. With no editor open in the process, a picture starts bgfx headless and FunkGui holds
    // it for the next (funkgui/gpu/GpuPicture.h, "The context"). A plugin lives in its host's process, so the hold is
    // let go once no picture has been drawn for `seconds` (kPictureHoldSeconds: a burst of pictures, as Claude Desktop
    // takes a page after a page, starts bgfx once, and none is kept for longer). The processor's timer calls it
    // (message thread only); nothing to do while nothing is held. Defined in Source/editor/Picture.cpp.
    inline constexpr double kPictureHoldSeconds = 10.0;
    void picturesIdle(double seconds = kPictureHoldSeconds);
    // A netlist's view as the session keeps it (editor/SchematicView.h): its Netlist or Schematic view, the drawing's
    // zoom (1: 100 %) and the world point at its canvas's centre (grid units), and the nets its probes are pinned to
    // (s6). Kept only for files whose view the user chose (a switch, a zoom, a pan), at most kMaxFileViews, each with at
    // most kMaxFileProbes; never in the Draft, a project or an export, never sent to Claude.
    inline constexpr std::size_t kMaxFileViews = 12;
    inline constexpr std::size_t kMaxFileProbes = 4;
    struct FileView
    {
        std::string file;                        // a .cir file's name
        bool schematic = false;                  // its Schematic view (else its Netlist view)
        float zoom = 1.0f;                       // 0: none chosen yet (the drawing fits when the view opens)
        float x = 0.0f, y = 0.0f;                // the world point at the canvas's centre
        std::vector<std::string> probes;         // pinned probes' node names (s6)

        friend bool operator==(const FileView&, const FileView&) = default;
    };

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

    // The Standalone window's close, minimise and zoom buttons, together (EditorPort::windowButtons).
    struct WindowButtons
    {
        float width = 0.0f, height = 0.0f;       // window points; 0 x 0: none over the editor
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
        virtual void check(const Sources& files) { (void)files; }
        virtual CheckStatus checkStatus() const { return {}; }
        virtual void addIrViewer() {}
        virtual void removeIrViewer() {}
        virtual IrStatus ir() const { return {}; }
        virtual uint64_t irRevision() const { return 0; }
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

        // The netlists' views (FileView, above), kept in the session: an editor writes them only on its user's
        // gestures (switching a view, zooming, panning), never when it opens or fits, and reads them when it opens.
        virtual std::vector<FileView> fileViews() const = 0;
        virtual void setFileViews(const std::vector<FileView>& views) = 0;

        // A note from the panel's keyboard (a .gui file's keyboard element): `velocity` 0..1 plays it, 0 releases it.
        // An instrument plays it at the start of its next block; an effect ignores it. 64 can wait at once; past that
        // a note is dropped.
        virtual void playNote(int note, float velocity) = 0;
        // The keyboard's wheels: the pitch wheel (`pitch`, -1..1) or the mod wheel (0..1), as MIDI's would be.
        virtual void playWheel(bool pitch, float value) = 0;

        // The Standalone's own window (plugin/standalone/) draws no title bar: its close, minimise and zoom buttons sit
        // over the top bar, which keeps them room. In a host's window none of this applies: windowButtons() is 0 x 0
        // and the other two do nothing.
        //   windowButtons()       the three buttons' size together, in window points (the editor's px at 100 %)
        //   placeWindowButtons()  where the top bar keeps their room, in the editor's logical px: its left edge and its
        //                         middle's height (the window places them there, at the editor's zoom)
        //   dragWindow()          a press on the top bar where nothing is (`clicks` 2: a double-click's second press):
        //                         the window drags, or zooms or minimises as the system's setting says
        virtual WindowButtons windowButtons() const { return {}; }
        virtual void placeWindowButtons(float x, float middle) { (void)x; (void)middle; }
        virtual void dragWindow(int clicks) { (void)clicks; }

        // The Standalone's audio and MIDI settings, a card of the editor's own (editor/SettingsCard.h) in place of JUCE's
        // dialog. In a host's window there are none: devices() is null (no card, no muted notice) and settingsAsked()
        // never moves.
        //   devices()        the devices, the choices and the actions (DevicePort.h); the window hands it over with its
        //                    buttons (Processor::WindowHooks), so ask again rather than keep it
        //   settingsAsked()  a count the app raises each time it asks for the card (Settings... in its menu, Cmd-,): the
        //                    editor opens the card when the count moves (a Panel polls it, starting from the count it
        //                    found when it was made)
        virtual DevicePort* devices() { return nullptr; }
        virtual uint64_t settingsAsked() const { return 0; }
    };
}
