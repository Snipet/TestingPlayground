// Source/plugin/EditorPort.h: what the editor reaches besides the parameter ports (JUCE-free; message thread only).
// The processor implements it (Processor.h); the Panel takes it as it takes the ParamPorts, so the editor never
// includes the processor.
//
//   draft()           the editor's files and the tab it shows, kept by the processor (and in the session) so they
//                     outlive the editor's window
//   draftRevision()   bumps when the draft changes other than by setDraft (a host loading a session), so an open editor
//                     shows the new files
//   setDraft()        the editor's files, as they are typed, and its tab
//   setDraftIf()      setDraft only while draftRevision() is still `revision` (the editor has every change merged): a
//                     schematic gesture's files, at once, so that Claude's conditional write (edit_circuit) sees it
//                     and a write of Claude's not yet merged is never overwritten (false: nothing set)
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
//   schematicPins()   each netlist's session pins, the places its Schematic view drew for parts and terminals with no
//                     line of their own, set with setSchematicPins() whenever the view reconciles; in memory only
//   panelView()       the editor shows the PANEL view (the interface across the left column) rather than the CODE view;
//                     kept in the session, set with setPanelView()
//   codePanel()       the CODE view shows the program's panel under the editor (its dials, or its .gui file's
//                     interface), one choice while the program's code shows and one while a file of its interface does;
//                     kept in the session, set with setCodePanel()
//   explorerWidth()   the explorer's and the chat's widths as the user set them (editor/Columns.h), kept in the session;
//   chatWidth()       0: none set
//   codeWindow()      the code editor and the explorer in a window of their own (editor/Panel.h, "the code window"):
//                     whether it is out, its size and its place, kept in the session, set with setCodeWindow();
//                     codeWindowOpen() whether an editor's code is in it now (addCodeWindow(), removeCodeWindow())
//   folder()          the program's files mirrored to a folder on disk for VS Code or any editor, synced both ways
//                     (plugin/FolderSync.h): on or off, its name and path, run on save, the conflicts and the notes;
//                     setFolderSync(), setFolderRunOnSave(), resolveFolderConflict(), openFolder() (the Finder, VS Code)
//   program()         the compile it came from (CompileStatus::request): the meters' taps are its fields once
//                     Meters::program says so (until the audio thread takes it, they are the last program's)
//   meters()          the last block's levels and the program's taps, for the panel's displays (Engine::Meters); false
//                     while the audio thread publishes (keep the last)
//   scope()           the last n samples of the input or the output (Engine::scope)
//   addMeterReader()  meters() and scope() are measured only while someone reads them (Engine::readers): a Panel adds
//   removeMeterReader() itself in its constructor and removes itself in its destructor; meterReaders() how many now
//   addProbeOwner()   the schematic's live probes (Engine.h, "Probes"): a Panel that arms one becomes an owner (a
//   setProbes()       headless picture's Panel never does), sets its ProbeSet (program 0: nothing) and removes itself
//   removeProbeOwner() when it goes; the newest owner whose set is armed reaches the Engine, so a second Panel that
//   probe()           arms nothing leaves the first's armed; probe() the last n samples of a slot's ring (Engine::probe)
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
//   startAfresh()     `files` as a new instance plays Init (EXAMPLES' example, RANDOM's program, each with the library
//                     files it #includes): they become the whole draft (every other file goes: a .gui, a netlist), they
//                     run, and every dial goes to its default once they compile. No project is open after it, no preset
//                     applied, Claude's checks are cleared and the interface's pages are its first; `what` names them
//                     in the journal ("the example \"Init\""). The chat's conversation and the editor's own look (its
//                     size, theme, zoom, view, folds, widths) stay
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
//   ab()              A/B listening (AbState below; editor/AbCard.h): two sides, each the editor's files as they were
//                     when it began ("Current") or a saved version of a project, played in turn while the host plays
//                     (each a run of its files: the draft stays as it is), the louder turned down to the quieter's
//                     loudness. startAb() begins one (A plays first, unless B is what plays already), setAbSides()
//                     changes its sides, playAb() plays a side, setAbOwnDials() gives each side its own dials (a
//                     version's saved ones, Current's as they were; each side's moves kept while it lasts) or the same
//                     dials for both, setAbMatch() turns the matched loudness on or off. keepAb() makes a side current
//                     (its files the draft: an open editor merges them as one undo step; a version's the project open)
//                     and ends it; endAb() ends it as it was before: what played then plays again, and its dials
//                     when each side had its own. A program that runs from elsewhere (Claude's, a session loaded) ends
//                     it too (AbState::ended says so). abRevision() is ab().revision, cheaply
//   chat()            the chat with Claude (ChatPort.h)
//   windowButtons()   the Standalone's own window: its buttons over the top bar, their place, its drag (below)
//   devices()         the Standalone's audio and MIDI devices (DevicePort.h), for the settings card; null in a host
//   settingsAsked()   bumps each time the Standalone asks for the settings card (its menu's Settings..., Cmd-,)
#pragma once

#include <array>
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
#include <utility>
#include <vector>

namespace funkgui
{
    class ParamPort;
}

namespace playground
{
    class EditorPort;

    // The machine, as the panel and the chat name it to the user ("stored encrypted on this Mac").
#if defined(_WIN32)
    inline constexpr const char* kThisMachine = "this computer";
#else
    inline constexpr const char* kThisMachine = "this Mac";
#endif

    // A shortcut as the user's keyboard names it: the text's "Cmd-" is "Ctrl-" on Windows, whose editor host gives
    // Ctrl the editor's Cmd (editor/nogpu/SoftEditorHost.h).
    inline std::string shortcutText(std::string_view macText)
    {
        std::string s(macText);
#if defined(_WIN32)
        for (std::size_t at = s.find("Cmd-"); at != std::string::npos; at = s.find("Cmd-", at + 5))
            s.replace(at, 4, "Ctrl-");
#endif
        return s;
    }

    // A picture of the editor, for the chat's view_panel tool: a Panel of its own over the processor's ports and its
    // EditorPort, laid out at the editor's size and drawn headless on the GPU, shaders as the screen draws them
    // (editor/Picture.h; where the GPU cannot, with SoftRaster, a shader as a striped placeholder, and `about` says so
    // and why), as PNG bytes. `window`: the whole editor as the user has it; else the program's panel in the PANEL
    // view, cropped to it. `page`: a tabs element's page to show, by its title ("" for the user's). The view, the chat's
    // fold, the pages and the probes the user has stay as they were (its Panel owns no probe: Panel::setOwnsProbes).
    // Message thread only. Defined in Source/editor/Picture.cpp.
    struct PanelPicture
    {
        std::string png;                         // the PNG's bytes ("" when it could not be made: `why`)
        int width = 0, height = 0;               // its pixels
        std::string about;                       // what it shows (the view, its size, the pages there are, the shaders)
        std::string why;
        bool gpu = false;                        // drawn by the GPU; false: SoftRaster (`about` says why)
    };
    // A panel state a picture draws in place of what plays now (view_panel's values and at): plain data, made on the
    // agent's worker, read on the message thread.
    struct PictureValue
    {
        std::string name;                        // a dial's, a field's (a display's source or name, a live= mark's or
                                                 // a shader's input: rpm, shared->gear), or in or out
        float number = 0.0f;                     // a dial's plain value, a field's value, a level's dBFS
        std::string text;                        // else a dial's option by its name ("TRUCK", "on")
        bool isText = false;
    };
    struct PictureState
    {
        std::vector<PictureValue> values;        // drawn in place of what the program gives (over at's state)
        // at's: an offline render of the program that plays (ChatAgent's view_panel) up to a moment: the meters one a
        // frame, 60 a second, oldest first, the last at that moment (at most a field scope's 2.5 s); the scopes' last
        // samples then, oldest first; the dials then (plain, by name); the render in words.
        static constexpr int kScopeSamples = 2048;   // the scopes' samples a panel reads (DisplayItem::kScopeSamples)
        std::vector<Meters> frames;
        std::vector<float> scopeIn, scopeOut;
        std::vector<std::pair<std::string, float>> dials;
        Sources sources;                         // the files it rendered: the picture refuses a panel of others
        std::string about;                       // "1.50 s into an offline render of the program that plays (...)"
        bool rendered() const noexcept { return !frames.empty(); }
        bool empty() const noexcept { return values.empty() && frames.empty(); }
    };
    // With a `state` (PictureState::empty: none), the picture draws it in place of what plays: the displays (the
    // fields, the audio's levels and scopes, at's frames played one a frame up to the last, which it shows) and the
    // dials; `about` then gives what the displays read and the names that are no dial or field of the panel.
    PanelPicture picturePanel(std::span<funkgui::ParamPort* const> ports, EditorPort& port, int width, int height,
                              bool window, const std::string& page, const PictureState& state = {});
    // What a picture shows (ChatHost::chatPicture): the program's panel, the whole editor, or a netlist's schematic.
    enum class PictureView : uint8_t { panel, window, schematic };
    // A netlist's schematic (view_panel's view "schematic"): the .cir file `file` of the port's draft, reconciled with
    // the port's session pins (schematicPins: the places the user's view drew for parts and terminals without a line,
    // so the picture is the user's drawing), drawn at 100 % (10 px a grid unit; smaller only to fit 1,400 x 900
    // logical px), every name and value shown, the last compile's marks on it when that compile's netlist is this one
    // (its *@ lines aside), by a small Panel of its own that holds no editor Panel: it owns no probe, reads and writes
    // no file view, and changes nothing of the user's. `about` lists each part (its kind, value, place, orientation and
    // pins' points), each net's pins, the problems and the parts not drawn. Message thread only (Picture.cpp).
    PanelPicture pictureSchematic(EditorPort& port, const std::string& file);

    // The pictures' GPU context. With no editor open in the process, a picture starts bgfx headless and FunkGui holds
    // it for the next (funkgui/gpu/GpuPicture.h, "The context"). A plugin lives in its host's process, so the hold is
    // let go once no picture has been drawn for `seconds` (kPictureHoldSeconds: a burst of pictures, as Claude Desktop
    // takes a page after a page, starts bgfx once, and none is kept for longer). The processor's timer calls it
    // (message thread only); nothing to do while nothing is held. Defined in Source/editor/Picture.cpp.
    inline constexpr double kPictureHoldSeconds = 10.0;
    void picturesIdle(double seconds = kPictureHoldSeconds);
    // A netlist's view as the session keeps it (editor/SchematicView.h): its Netlist or Schematic view, the drawing's
    // zoom (1: 100 %) and the world point at its canvas's centre (grid units), and the nets its probes are pinned to.
    // Kept only for files whose view the user chose (a switch, a zoom, a pan), at most kMaxFileViews, each with at most
    // kMaxFileProbes; never in the Draft, a project or an export, never sent to Claude.
    inline constexpr std::size_t kMaxFileViews = 12;
    inline constexpr std::size_t kMaxFileProbes = 4;
    struct FileView
    {
        std::string file;                        // a .cir file's name
        bool schematic = false;                  // its Schematic view (else its Netlist view)
        float zoom = 1.0f;                       // 0: none chosen yet (the drawing fits when the view opens)
        float x = 0.0f, y = 0.0f;                // the world point at the canvas's centre
        std::vector<std::string> probes;         // pinned probes' node names

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
        bool changed = false;                    // the editor's files differ from those last opened, saved or loaded
                                                 // (a version, an example, the session, a new instance's Init), with a
                                                 // project open or not: what loading another would lose
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

    // The code window (EditorPort::codeWindow): the code editor and the explorer in a top-level window of their own, the
    // main window showing the panel full size while it is out. Its size is the Panel's (logical px; 0 x 0: the default),
    // its place the window's top-left on the screen (points; `placed` false: none yet, it opens beside the main window).
    struct CodeWindow
    {
        bool out = false;
        int width = 0, height = 0;
        bool placed = false;
        int x = 0, y = 0;

        friend bool operator==(const CodeWindow&, const CodeWindow&) = default;
    };

    // The folder the program's files are mirrored to, for VS Code or any editor (EditorPort::folder; plugin/FolderSync.h).
    struct FolderState
    {
        bool on = false;
        std::string name;                        // the folder's ("Synth 1", or the project's)
        std::string path;                        // where it is, "~/..." for the home folder
        bool runOnSave = false;                  // a save there runs the program
        bool watching = false;                   // the system says when it changes (else it is looked at twice a second)
        std::vector<std::string> conflicts;      // files changed on both sides since they last agreed: held, neither
                                                 // side written, until the user keeps one (resolveFolderConflict)
        std::vector<std::string> notes;          // what it did and did not take, and why, short: the newest first
        std::string landed;                      // the files the last save in the folder changed ("program.fm")
        uint64_t landedCount = 0;                // bumps with each save that lands
        std::string error;                       // why it is not on ("" when it is, or was turned off)
    };
    // Where a folder opens (EditorPort::openFolder).
    enum class FolderApp : uint8_t { files, vscode };   // the Finder (Windows' Explorer), or VS Code

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

    // A/B listening (EditorPort::ab): its two sides and how it plays them.
    struct AbSide
    {
        int version = 0;                         // the version's number in the project; 0: Current, the editor's files
                                                 // as they were when it began
        std::string label;                       // "Current", "v3"
        std::string note;                        // the version's note ("program.fm +12 -3"), or what Current is
        Sources files;
    };
    struct AbState
    {
        bool on = false;
        std::string project;                     // whose versions the sides are ("" for none: Current alone)
        std::array<AbSide, 2> sides{};           // A and B
        int playing = -1;                        // the side that plays: 0 A, 1 B; -1 neither yet
        int pending = -1;                        // the side whose compile is on its way (-1: none)
        bool ownDials = false;                   // each side its own dials; else the same dials for both
        bool match = true;                       // the louder side turned down to the quieter's loudness
        bool measuring = false;                  // the loudness is being measured (offline, off the message thread),
                                                 // or the dials moved since and it is measured again once they rest
        std::array<double, 2> lufs{ 0.0, 0.0 };  // each side's loudness as measured (BS.1770, LUFS); NaN until then
        std::array<float, 2> trimDb{ 0.0f, 0.0f };   // the output's gain while that side plays (<= 0 dB; 0 unmatched)
        std::string why;                         // one line: a side that does not compile
        std::string unmatched;                   // why the loudness is not matched, once measured ("" when it is)
        std::string ended;                       // why it ended when it ended on its own (another program ran, a
                                                 // session loaded), else ""
        uint64_t revision = 0;                   // bumps whenever any of the above changes
    };

    class EditorPort
    {
    public:
        virtual ~EditorPort() = default;

        virtual Draft draft() const = 0;
        virtual uint64_t draftRevision() const = 0;
        virtual void setDraft(const Draft& draft) = 0;
        virtual bool setDraftIf(const Draft& draft, uint64_t revision) = 0;

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
        // The probes (above): an owner's id (from 1), its set, its leaving; a slot's last n samples, oldest first: how
        // many are of the set that plays (Engine::probe).
        virtual int addProbeOwner() { return 0; }
        virtual void setProbes(int owner, const ProbeSet& set)
        {
            (void)owner;
            (void)set;
        }
        virtual void removeProbeOwner(int owner) { (void)owner; }
        virtual int probe(int slot, float* out, int n) const
        {
            (void)slot;
            for (int i = 0; i < n; ++i)
                out[i] = 0.0f;
            return 0;
        }

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
        virtual void startAfresh(const Sources& files, const std::string& what) = 0;
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
        // A/B listening (AbState). Sides are version numbers of `project` (0: Current). False, with why, when a version is
        // not kept (startAb, setAbSides) or none is on (keepAb).
        virtual AbState ab() const = 0;
        virtual uint64_t abRevision() const = 0;         // AbState::revision, without the copy
        virtual bool startAb(const std::string& project, int a, int b, std::string& why) = 0;
        virtual bool setAbSides(int a, int b, std::string& why) = 0;
        virtual void playAb(int side) = 0;
        virtual void setAbOwnDials(bool own) = 0;
        virtual void setAbMatch(bool on) = 0;
        virtual bool keepAb(int side, std::string& why) = 0;
        virtual void endAb() = 0;
        // Claude Desktop (plugin/McpLink.h): this instance's name to it ("Synth 2"; "" when there is no link), and
        // whether Claude Desktop is talking to it now.
        virtual std::string desktopName() const = 0;
        virtual bool desktopConnected() const = 0;
        virtual ChatPort& chat() = 0;

        virtual bool panelView() const = 0;
        virtual void setPanelView(bool on) = 0;
        // Whether the CODE view shows the program's panel (its dials, or its .gui file's interface) under the editor:
        // `gui` the choice while a file of the interface shows (a .gui file, a shader's .sc: editor/Items.h's
        // isInterfaceFile), else the one while the program's code shows (its FMinus files, a netlist, the IR). Both
        // shown at first; kept in the session (the editor's status line's Panel switch and Cmd-J set them).
        virtual bool codePanel(bool gui) const = 0;
        virtual void setCodePanel(bool gui, bool shown) = 0;

        // Whether the chat's column is open (else folded into a rail at the right), kept in the session.
        virtual bool chatOpen() const = 0;
        virtual void setChatOpen(bool open) = 0;
        // Whether the explorer (the program's files, the library, the examples and the presets: editor/Explorer.h) is
        // open at the left (else folded into a rail), kept in the session.
        virtual bool explorerOpen() const = 0;
        virtual void setExplorerOpen(bool open) = 0;
        // The explorer's and the chat's widths as the user set them (a drag of the explorer's right edge or the chat's
        // left edge, their keys: editor/Columns.h), logical px, kept in the session; 0: none set (the editor then takes
        // this Mac's default for an instance that has set none, else a share of the window).
        virtual int explorerWidth() const = 0;
        virtual void setExplorerWidth(int px) = 0;
        virtual int chatWidth() const = 0;
        virtual void setChatWidth(int px) = 0;
        // The code window (CodeWindow, above), kept in the session: the editor's window button and the window's close
        // set `out`, the window its size and place as the user moves it. The plugin's editor follows `out` (it opens or
        // closes the window); a Panel drawn headless has no window and shows the editor in place.
        virtual CodeWindow codeWindow() const { return {}; }
        virtual void setCodeWindow(const CodeWindow& window) { (void)window; }
        // Whether an editor's code is in its code window now: its Panel adds itself when it lends the code window the
        // editor and removes itself when it takes it back or goes. view_panel's picture shows this, not the session's
        // choice, which an editor follows only once it is on screen (and one without a window's owner never does). In
        // the processor's memory only.
        virtual void addCodeWindow() {}
        virtual void removeCodeWindow() {}
        virtual bool codeWindowOpen() const { return false; }

        // The folder (FolderState, above): the program's files mirrored to a folder on disk and synced both ways, kept
        // in the session (its name and run on save). setFolderSync turns it on (a folder of the project open's name,
        // else the instance's; false with why when none could be made) or off (the folder stays as it is);
        // folderRevision bumps when folder() changes. resolveFolderConflict keeps one side of a conflict (the other in
        // the folder's .conflicts/). openFolder opens it in the Finder or VS Code (false with why: VS Code missing).
        virtual FolderState folder() const { return {}; }
        virtual uint64_t folderRevision() const { return 0; }
        virtual bool setFolderSync(bool on, std::string& why)
        {
            (void)on;
            why = "no folder here";
            return false;
        }
        virtual void setFolderRunOnSave(bool on) { (void)on; }
        virtual bool resolveFolderConflict(const std::string& file, bool keepPlugin, std::string& why)
        {
            (void)file;
            (void)keepPlugin;
            why = "no folder here";
            return false;
        }
        virtual bool openFolder(FolderApp app, std::string& why)
        {
            (void)app;
            why = "no folder here";
            return false;
        }

        // The pages the panel's interface shows: each tabs element's chosen page (the tabs in the order the .gui has
        // them), by its title. Kept in the session, so a reopened editor and a reloaded session show them again.
        virtual std::vector<std::string> guiPages() const = 0;
        virtual void setGuiPages(const std::vector<std::string>& titles) = 0;

        // The netlists' views (FileView, above), kept in the session: an editor writes them only on its user's
        // gestures (switching a view, zooming, panning), never when it opens or fits, and reads them when it opens.
        virtual std::vector<FileView> fileViews() const = 0;
        virtual void setFileViews(const std::vector<FileView>& views) = 0;

        // The schematic's session pins (editor/SchematicView.h): for each netlist, the places its view drew for parts
        // and terminals that have no *@ line, as *@ part, input and output lines ("" for none). A Panel sets them
        // whenever its view reconciles the file; they live in the processor's memory only (never in the session, a
        // project or an export), and Claude's edit_circuit (ChatHost::chatSchematicPins) and view_panel's schematic
        // picture reconcile with them, so that every writer and every viewer draws what the user sees.
        virtual void setSchematicPins(const std::string& file, const std::string& lines) { (void)file; (void)lines; }
        virtual std::string schematicPins(const std::string& file) const
        {
            (void)file;
            return {};
        }

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
