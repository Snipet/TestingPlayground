// Source/plugin/Processor.h: the JUCE AudioProcessor. It plays an FMinus program that the editor's text field holds
// (README.md, "Writing a program"): the Engine (Source/dsp/Engine.h) runs it, the Compiler (Source/dsp/Compiler.h)
// compiles what the editor runs, and the processor owns the rest:
//   - thirty-two host parameters, P1 to P32, normalised 0..1, which the running program's first 32 parameters take
//     (Controls.h). Each is a ControlParameter: its name and its text are the program's ("2 cutoff", "1200.0"), and a
//     host is told when they change (parameterInfoChanged). One FunkGui ParamPort each, what the editor's dials write
//     through. The next 32 parameters are the panel's own dials (Controls.h's isPanelDial): no host parameter, their
//     values the processor's (panelValues_, kept in the session as <PANEL><DIAL slot=".." value=".."/></PANEL>), a
//     port each as well; everything else (the carry, Claude, the projects and presets, A/B) treats both alike
//     (writeDial). A run that brings a parameter the program did not have in that slot moves it to its default, and one
//     of the name the slot had keeps its value in its units (Controls.h: clamped to a new range, an int's option by
//     its name), from the new program's first block (the dials' carry: Compiler.h, Engine.h, apply());
//   - the program: the files of the one that plays, and the editor's draft (its files, a tab each, and the tab it
//     shows), both kept in the session (a <PROGRAM active=".."> beside the parameters, with a <RUN> and a <DRAFT> of
//     <FILE name=".." text=".."/>s). A new instance plays the first of the EXAMPLES (Init); a session loads its program
//     (compiled at once, on the host's thread) and its dials as it saved them. A session of the version before files
//     (<PROGRAM source=".." draft=".."/>) loads as a program of one file;
//   - effect or instrument (the product's KIND, products/<key>.cmake: product::kIsInstrument): an effect has a
//     stereo input and output and processes in place; an instrument has an output only and gets the block's MIDI
//     notes as Events;
//   - the chat with Claude (ChatAgent.h): its conversations are kept on this Mac only (ChatStore.h), with the project
//     open (by its id) or, while none is, with this instance; the session names the chat (<CHAT instance=".."
//     owner=".." chat=".."/>), never holds it. Its tools act on the draft, RUN and the dials through ChatHost, from the
//     agent's thread (the draft under programMutex_, a dial queued for poll(); its projects tool's saves, opens and
//     presets a job poll() runs: ProcessorSaves.cpp).
//   - the editor's size, as the user left it (the editor is resizable), its view, its folds, the widths its user set
//     for the explorer and the chat, and its interface's pages, kept in the session (<EDITOR w=".." h=".."
//     view="panel" chat="closed" explorerW=".." chatW=".."> with a <PAGE title=".."/> a tabs element);
//   - the folder (FolderSync.h): the program's files mirrored to a folder under the projects' (Folders/<name>) for VS
//     Code or any editor, synced both ways on the timer, kept in the session while it is on (<FOLDER name=".."
//     run="1"/>; a state without one leaves the folder as it is).
//   - A/B listening (ProcessorAb.cpp; EditorPort::ab): two versions of a project, or the editor's files and a
//     version, played in turn as runs of their files, the louder turned down on the Engine's output (AbMatch.h measures
//     them offline), until one is kept (its files the draft) or it ends. Not in the session itself: a session saved
//     while it is on keeps what played before it began (and those dials, when each side has its own), as Close would
//     put back, and a session loaded ends it.
//   - the samples (dsp/Assets.h; EditorPort::assets, the explorer's ASSETS): audio files a program reads as tables
//     (#pragma sample), decoded when added (AssetFiles.h, on the message thread or Claude's worker), never on the audio
//     thread; the set the Compiler and every tool's render compile with. Kept in the session as each file was added
//     (<ASSETS><ASSET name=".." data="base64"/></ASSETS>; a state without them has none) and with each project version
//     saved (ProjectStore.h: opening a version makes its samples the plugin's); a fresh start (EXAMPLES, RANDOM) keeps
//     them, as the user's material for the next program.
//   - MIDI learn (MidiLearn.h; the instrument's): a dial learns a MIDI controller's control, which then moves it
//     through its port as the editor's hand does (a host gesture a touch). The audio thread notes the control changes
//     (learn_), the message thread writes them (pollMidi, on a timer of its own while there is anything to listen for).
//     The mappings are kept in the session (<MIDI><MAP dial=".." channel=".." cc=".." fine="1"/></MIDI>; a state
//     without them has none) and with each project version saved (ProjectStore.h).
//   - the presets in turn (EditorPort::presetCycle, stepPreset): the project open's presets, which the top bar steps
//     through and which are the host's programs (getNumPrograms ...: a VST3's list a fixed kVst3Programs, the rest the
//     presets themselves) and what MIDI's program change on channel 1 chooses (the synth's). Applied as any preset is.
//
// What a host may do, and what the Engine sees of it:
//   - a block larger than prepareToPlay announced: the Engine gets it in parts of at most that size;
//   - a block of 0 samples (JUCE's VST3 wrapper passes them on): an instrument's Engine still gets its events;
//   - more than kMaxEventsPerBlock events in a block: the block is played in parts, cut at the event that does not
//     fit, so no event is lost;
//   - MIDI: note-on (at velocity 0 a note-off), note-off, the controllers All Sound Off (120) and All Notes Off (123),
//     both an allNotesOff event, the mod wheel (1) and the sustain pedal (64), pitch bend and channel pressure, and a
//     program change on channel 1 (a preset of the project open, applied by poll()); the rest is ignored. The host's transport (tempo, position, signature, playing) goes to the Engine once a block,
//     for the program's Host block (host.fm). The panel's keyboard's notes (playNote) come
//     through a lock-free queue and play at the start of the next block, before its MIDI;
//   - bypass (processBlockBypassed; there is no bypass parameter): each edge is a 20 ms smootherstep crossfade between
//     the Engine's output and the bypassed sound (an effect's input; an instrument's silence), and the Engine keeps
//     running through the bypass, unheard (an instrument on the note-offs only), so un-bypassing fades in a state that
//     followed the host;
//   - processBlock before any prepareToPlay: the Engine is not called; an effect passes its input through, sanitised
//     (NaN and inf as 0), an instrument is silent;
//   - a NaN written to a parameter: the Engine keeps the last good value, and the host's text for it is a dash.
//
// Threads: the audio thread reads the parameters' atomics once a block and calls the Engine; it never allocates,
// locks, or touches the editor or the compiler. The editor (message thread) reaches the processor only through ports()
// and editorPort(). The compiler's outcome reaches the message thread through poll(), which a timer calls (20 Hz)
// and the probes call themselves; it also clears the alerts after two seconds.
#pragma once

#include "plugin/AbMatch.h"
#include "plugin/AssetFiles.h"
#include "plugin/Capture.h"
#include "plugin/ChatAgent.h"
#include "plugin/EditorPort.h"
#include "plugin/FactoryStore.h"
#include "plugin/FolderSync.h"
#include "plugin/Journal.h"
#include "plugin/McpLink.h"
#include "plugin/MidiLearn.h"
#include "plugin/ProjectStore.h"

#include "dsp/Compiler.h"
#include "dsp/Controls.h"
#include "dsp/Engine.h"
#include "dsp/Events.h"

#include "fsdsp/core/Ramp.h"

#include <funkgui/params/JuceParamPort.h>
#include <funkgui/params/ParamPort.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace playground
{
    namespace preview
    {
        struct Prepared;                                         // editor/Preview.h (package 04)
        struct Request;
    }

    class Processor final : public juce::AudioProcessor, private EditorPort, private ChatHost, private juce::Timer
    {
    public:
        Processor();
        ~Processor() override;

        const juce::String getName() const override;
        bool acceptsMidi() const override;
        bool producesMidi() const override;
        bool isMidiEffect() const override;
        double getTailLengthSeconds() const override;

        bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
        void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
        void releaseResources() override;
        void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
        void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
        using juce::AudioProcessor::processBlock;               // the double-precision overloads keep JUCE's default
        using juce::AudioProcessor::processBlockBypassed;

        bool hasEditor() const override;
        juce::AudioProcessorEditor* createEditor() override;

        int getNumPrograms() override;
        int getCurrentProgram() override;
        void setCurrentProgram(int index) override;
        const juce::String getProgramName(int index) override;
        void changeProgramName(int index, const juce::String& newName) override;

        void getStateInformation(juce::MemoryBlock& destData) override;
        void setStateInformation(const void* data, int sizeInBytes) override;

        // ---- for the editor (message thread) and the probes ------------------------------------------------------
        juce::AudioProcessorValueTreeState& parameters() noexcept { return apvts_; }
        std::span<funkgui::ParamPort* const> ports() const noexcept { return ports_; }   // P1 to P32, then the panel's
        EditorPort& editorPort() noexcept { return *this; }
        ChatAgent& chatAgent() noexcept { return chat_; }        // probes: its transport, its turns
        ChatHost& chatHost() noexcept { return *this; }          // probes: what Claude's tools act on
        // The editor's size (logical px) as the user left it, kept in the session; 0 x 0: the Panel's default.
        int editorWidth() const noexcept { return editorW_.load(); }
        int editorHeight() const noexcept { return editorH_.load(); }
        void setEditorSize(int w, int h) noexcept;
        // The Standalone's own window (plugin/standalone/), which the editor reaches through EditorPort::windowButtons,
        // placeWindowButtons and dragWindow: its buttons' size, and what it does with their place and a drag; and the
        // app's audio and MIDI devices for the settings card (EditorPort::devices; the app keeps them, and lets go of
        // the hooks before they go). Set by the window (and the probes), message thread; never set in a host's window.
        struct WindowHooks
        {
            WindowButtons buttons;
            std::function<void(float x, float middle)> place;
            std::function<void(int clicks)> drag;
            DevicePort* devices = nullptr;
        };
        void setWindowHooks(WindowHooks hooks);
        // The app's Settings... (Cmd-,): the editor opens its settings card at its next frame (EditorPort::settingsAsked
        // moves), at `page` (a page's id, SettingsPages.h: the Audio and MIDI page's "audio" as the app asks; "" the
        // card's own choice). Message thread.
        void showSettings(std::string page = "audio")
        {
            settingsPage_ = std::move(page);
            ++settingsAsked_;
        }
        Engine& engine() noexcept { return engine_; }
        Compiler& compiler() noexcept { return compiler_; }   // for the probes
        // The folder (EditorPort::folder): its sync (the probes pass it at a clock of their own), and what opens it in
        // the Finder or VS Code (EditorPort::openFolder; the probes' records the call, launching nothing; the default
        // launches nothing under a probe's sandbox either). Message thread.
        FolderSync& folderSync() noexcept { return folder_; }
        using FolderLauncher = std::function<bool(FolderApp app, const std::string& path, std::string& why)>;
        void setFolderLauncher(FolderLauncher launcher) { folderLauncher_ = std::move(launcher); }
        capture::Recorder& recorder() noexcept { return capture_; }   // the capture tool's recorder (the probes)
        // The most samples one Engine::process call got since prepareToPlay (never more than it announced).
        int largestEngineCall() const noexcept { return largestCall_; }
        // The compiler's outcome to the message thread (the timer's call); settle() waits for a run first. It also makes
        // the picture Claude's view_panel asks for, there.
        void poll();
        void settle();                                           // a save in flight (startSave) lands too
        // Package 04's previews (ProcessorPreviews.cpp), for the probes: how many poll() drew, how many the table holds,
        // whether one waits to be prepared or drawn; previewsSettle() polls until none waits (a generous deadline).
        static constexpr std::size_t kPreviewsKept = 12;
        uint64_t previewsDrawn() const noexcept { return previewsDrawn_.load(); }
        std::size_t previewsKept() const;
        bool previewsWaiting() const;
        std::size_t previewsQueued() const;                      // waiting for previewThread_
        std::size_t previewsToDraw() const;                      // prepared, waiting for poll()
        bool pictureWaiting() const;                             // a picture for Claude asked for, not drawn yet
        bool previewsSettle(double seconds = 60.0);
        // For the probes: the editor's save in flight (startSave) waits before the store's work while shut, so that what
        // shows meanwhile can be checked; open, it goes on (poll() lands it). saveWritten: the last one started has done
        // its store's work (the next poll() lands it), so a probe can time the landing alone.
        void setSaveGate(bool shut);
        bool saveWritten() const noexcept { return saverDone_.load(std::memory_order_acquire); }
        // For the probes: the export in flight (startExport) waits before its store's work while shut (a cancel lets it
        // go, to end at once), so that what the message thread does meanwhile can be counted.
        void setExportGate(bool shut);
        // For the probes: a job of Claude's projects tool waits for poll() (ProcessorSaves.cpp), and how long one waits
        // before it is withdrawn (kClaudeJobSeconds; set before a call, from the message thread).
        bool claudeJobWaiting() const;
        void setClaudeJobWaitForTests(int ms) noexcept { claudeJobMs_ = ms > 0 ? ms : kClaudeJobSeconds * 1000; }

        // Who reads the meters and the scopes (Engine::readers): the audio thread measures them only while someone does.
        // The editor's Panel is a reader while it lives (EditorPort); a tool that listens to the plugin (Claude
        // Desktop's) adds itself while it listens and removes itself after. Any thread.
        void addMeterReader() noexcept override { engine_.addReader(); }
        void removeMeterReader() noexcept override { engine_.removeReader(); }
        int meterReaders() const noexcept override { return engine_.readers(); }

        // Slot c's dial as the program that plays has it (ControlParameter's name and text).
        Control control(int c) const;
        std::vector<std::string> controlOptions(int c) const;   // its values' names (#pragma panel options), maybe none

        // MIDI learn (MidiLearn.h): the control changes noted since, learned and written as the dials' gestures, a
        // touch's gesture ended kTouchMs after its last. `nowMs` a clock in ms: its timer's, a probe's own. Message thread.
        void pollMidi(double nowMs);
        uint64_t midiGestures() const noexcept { return midiGestures_; }   // the slots in a gesture of MIDI's (probes)

    private:
        static BusesProperties buses();                          // the kind's buses (product::kIsInstrument)
        juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

        void hostBlock(juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, bool bypassed) noexcept;
        void readParameters() noexcept;                          // the dials' values, to the Engine
        void play(juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, bool bypassed) noexcept;
        void keepDry(float* const* channels, int numChannels, int numSamples) noexcept;
        void bypassMix(float* const* channels, int numChannels, int numSamples) noexcept;
        void runUnprepared(juce::AudioBuffer<float>& buffer) noexcept;

        // poll()'s work: a finished compile's program and dials. A run's (`run`): its carry written to the host's
        // parameters (Controls.h's DialCarry: each dial in its units, a new one at its default), but for the slots in
        // `given`, which take those plain values (a version's or a preset's dials, a fresh start's defaults); a host's
        // state load's: none (its values come with it). `loaded`: a program no run played (the constructor's Init, a
        // state's), so nothing played before it (chatPrevious).
        struct Given
        {
            uint64_t slots = 0;                                  // slotBit(c)
            std::array<float, kMaxControls> plain{};
        };
        void apply(const CompileStatus& s, bool run, bool loaded, const Given& given);
        // The dials' carry (Compiler::setDialCarrier): how the host's values, in the controls of meaningProgram_, come
        // across to a program of `controls`. On the thread that offers.
        DialCarry carryFor(const Controls& controls, const PanelSpec& panel);
        // Whether a RUN now may be a panel's edit (Compiler.h, "A panel's edit"): not while an A/B is on. Any thread.
        bool panelEdits() const;
        // The carry of `s` (or `given`) written as host gestures, a slot whose value the host or the user moved since
        // left as it is; then the values' meaning is `s`'s program (unless a carry was made since: it writes instead).
        // A carry writes once: `s` applied again (a new revision, the same program) writes only `given`.
        void writeCarry(const CompileStatus& s, const Controls& controls,
                        const std::array<std::vector<std::string>, kMaxControls>& options, const Given& given);
        // The values' meaning set to a program whose values came with it (a state load), any carry made before stale.
        void adoptMeaning(const CompileStatus& s);
        // A version's or a preset's dials by name (each by its option's name first, ProjectStore.h's Dial), in units.
        static Given givenByName(const Controls& cs, const std::array<std::vector<std::string>, kMaxControls>& options,
                                 const std::vector<projectstore::Dial>& dials);
        void timerCallback() override;

        // EditorPort
        Draft draft() const override;
        uint64_t draftRevision() const override;
        void setDraft(const Draft& draft) override;
        bool setDraftIf(const Draft& draft, uint64_t revision) override;
        void run(const Sources& files) override;
        bool compiling() const override;
        CompileStatus status() const override;
        uint64_t statusRevision() const override;
        void check(const Sources& files) override;
        CheckStatus checkStatus() const override;
        void addIrViewer() override;
        void removeIrViewer() override;
        IrStatus ir() const override { return compiler_.ir(); }
        uint64_t irRevision() const override { return compiler_.irRevision(); }
        Sources running() const override;
        Controls controls() const override;
        PanelSpec panel() const override;
        uint64_t program() const override;
        bool meters(Meters& out) const override;
        void scope(bool input, float* out, int n) const override;
        int addProbeOwner() override;
        void setProbes(int owner, const ProbeSet& set) override;
        void removeProbeOwner(int owner) override;
        int probe(int slot, float* out, int n) const override { return engine_.probe(slot, out, n); }
        float load() const override;
        bool frozen() const override;
        RecordingState recording() const override;
        int markRecording(std::string& why) override;
        uint32_t alerts() const override;
        std::vector<ProjectEntry> projects() const override;
        std::vector<ProjectEntry> factoryProjects() const override;
        uint64_t factoryGeneration() const override;
        std::string projectFolder() const override;
        std::string tidyName(const std::string& typed) const override { return projectstore::tidyName(typed); }
        ProjectState project() const override;
        std::vector<VersionEntry> versions(const std::string& project) const override;
        std::vector<PresetEntry> presets(const std::string& project) const override;
        std::vector<ProjectEntry> projectsPolled() const override;
        std::vector<PresetEntry> presetsPolled(const std::string& project) const override;
        ProjectDetail projectPolled(const std::string& project) const override;
        uint64_t projectsRead() const override;
        bool saveVersion(const std::string& name, SavedVersion& saved, std::string& why) override;
        bool startSave(const std::string& name, std::string& why) override;
        SaveState saving() const override;
        bool openProject(const std::string& name, int version, std::string& why) override;
        void startAfresh(const Sources& files, const std::string& what) override;
        bool deleteProject(const std::string& name, std::string& why) override;
        bool deleteVersion(const std::string& name, int version, std::string& why) override;
        bool savePreset(const std::string& preset, std::string& why) override;
        bool applyPreset(const std::string& project, const std::string& preset, std::string& why) override;
        bool deletePreset(const std::string& project, const std::string& preset, std::string& why) override;
        bool setPresetCategory(const std::string& project, const std::string& preset, const std::string& category,
                               std::string& why) override;
        PresetCycle presetCycle() const override;
        bool stepPreset(int step, std::string& why) override;
        ClaudeNotice claudeNotice() const override;
        ProjectInfo projectInfo(const std::string& project) const override;
        bool setProjectInfo(const std::string& project, const ProjectInfo& info, std::string& why) override;
        bool setProjectCategory(const std::string& project, const std::string& category, std::string& why) override;
        ProjectsView projectsView() const override;
        bool setProjectsView(const ProjectsView& view, std::string& why) override;
        bool exportProject(const std::string& project, bool package, bool allVersions, const std::string& path, Exported& out,
                           std::string& why) override;
        bool importProject(const std::string& path, std::string& name, std::string& why) override;
        bool importProjects(const std::string& path, Imported& out, std::string& why) override;
        bool startExport(const std::vector<std::string>& projects, bool package, bool allVersions, const std::string& path,
                         std::string& why) override;
        ExportState exporting() const override;
        void cancelExport() override;
        std::string exportFolder() const override;
        // Package 05: projects and git (ProcessorGit.cpp).
        GitLink gitLink(const std::string& project) const override;
        bool startGit(const GitJob& job, std::string& why) override;
        GitState gitState() const override;
        void cancelGit() override;
        uint64_t gitPolled() const override;
        bool disconnectGit(const std::string& project, std::string& why) override;
        std::string gitChosen() const override;
        bool setGitChosen(const std::string& path, std::string& why) override;
        std::string gitFolder() const override;
        AbState ab() const override;
        uint64_t abRevision() const override { return ab_.st.revision; }
        bool startAb(const std::string& project, int a, int b, std::string& why) override;
        bool setAbSides(int a, int b, std::string& why) override;
        void playAb(int side) override;
        void setAbOwnDials(bool own) override;
        void setAbMatch(bool on) override;
        bool keepAb(int side, std::string& why) override;
        void endAb() override;
        std::string desktopName() const override { return mcp_ != nullptr ? mcp_->name() : std::string(); }
        bool desktopConnected() const override { return mcp_ != nullptr && mcp_->connected(); }
        DesktopState desktop() const override;
        bool setDesktopName(const std::string& name, std::string& why) override;
        void chooseDesktop(const std::string& id, bool on) override;
        ChatPort& chat() override { return chat_; }
        bool panelView() const override { return panelView_.load(); }
        void setPanelView(bool on) override { panelView_.store(on); }
        bool codePanel(bool gui) const override { return !(gui ? guiPanelHidden_ : codePanelHidden_).load(); }
        void setCodePanel(bool gui, bool shown) override { (gui ? guiPanelHidden_ : codePanelHidden_).store(!shown); }
        bool chatOpen() const override { return chatOpen_.load(); }
        void setChatOpen(bool open) override { chatOpen_.store(open); }
        bool explorerOpen() const override { return explorerOpen_.load(); }
        void setExplorerOpen(bool open) override { explorerOpen_.store(open); }
        int explorerWidth() const override { return explorerW_.load(); }
        void setExplorerWidth(int px) override { explorerW_.store(std::max(0, px)); }
        int chatWidth() const override { return chatW_.load(); }
        void setChatWidth(int px) override { chatW_.store(std::max(0, px)); }
        CodeWindow codeWindow() const override;
        void setCodeWindow(const CodeWindow& window) override;
        void addCodeWindow() override { ++codeWindows_; }
        void removeCodeWindow() override { codeWindows_.store(std::max(0, codeWindows_.load() - 1)); }
        bool codeWindowOpen() const override { return codeWindows_.load() > 0; }
        FolderState folder() const override;
        uint64_t folderRevision() const override { return folder_.revision(); }
        bool setFolderSync(bool on, std::string& why) override;
        void setFolderRunOnSave(bool on) override;
        bool resolveFolderConflict(const std::string& file, bool keepPlugin, std::string& why) override;
        bool openFolder(FolderApp app, std::string& why) override;
        void playNote(int note, float velocity) override;
        void playWheel(bool pitch, float value) override;
        std::vector<std::string> guiPages() const override;
        void setGuiPages(const std::vector<std::string>& titles) override;
        std::vector<FileView> fileViews() const override;
        void setFileViews(const std::vector<FileView>& views) override;
        void setSchematicPins(const std::string& file, const std::string& lines) override;
        std::string schematicPins(const std::string& file) const override;
        WindowButtons windowButtons() const override { return windowHooks_.buttons; }
        void placeWindowButtons(float x, float middle) override;
        void dragWindow(int clicks) override;
        DevicePort* devices() override { return windowHooks_.devices; }
        uint64_t settingsAsked() const override { return settingsAsked_; }

        // ---- Settings: the Debug page, a folder shown (ProcessorDebug.cpp; EditorPort's "Settings") --------------------
        std::string settingsPage() const override { return settingsPage_; }
        void addDebugViewer() override { ++debugViewers_; }
        void removeDebugViewer() override { debugViewers_ = std::max(0, debugViewers_ - 1); }
        int debugViewers() const override { return debugViewers_; }
        std::vector<DebugSection> debugRows() const override;
        uint64_t debugGathers() const override { return debugGathers_; }
        bool showPath(const std::string& path, std::string& why) override;
        bool midiLearnable() const override;
        midilearn::View midi() const override { return learn_.view(); }
        uint64_t midiRevision() const override { return learn_.revision(); }
        bool learnMidi(int slot) override;
        void cancelMidiLearn() override;
        bool forgetMidi(int slot) override;
        int forgetAllMidi() override;
        std::vector<AssetEntry> assets() const override;
        uint64_t assetsRevision() const override { return assetsRevision_.load(); }
        int addAssets(const std::vector<std::string>& paths, std::string& why) override;
        bool removeAsset(const std::string& name, std::string& why) override;

        // One version's content (EditorPort.h; ProcessorVersions.cpp): from the store's version table, the Reader reading
        // what it lacks; the editor's own, no read.
        VersionContent versionPolled(const std::string& project, int number) const override;
        VersionContent editorContent() const override;

        // ---- Package 04: panel previews (EditorPort.h's PreviewState; ProcessorPreviews.cpp) -------------------------
        // A version's panel as the Projects card shows it: the newest kPreviewsKept pictures in a table, a version not
        // in it read (versionPolled), prepared on previewThread_ (its dials, its .gui, its shaders: preview::prepare)
        // and drawn by poll() on the message thread, one a poll at most and none in a poll that drew a picture for
        // Claude (pollPreviews). The table is let go kPictureHoldSeconds after the last previewPolled.
        PreviewState previewPolled(const std::string& project, int number, int theme) override;

        // ChatHost: Claude's tools (ChatAgent.h), from the agent's worker thread.
        Draft chatDraft() const override;
        void chatSetDraft(const Draft& draft, const Draft& read) override;
        bool chatSetDraftIf(const Draft& draft, const Sources& read) override;
        std::string chatSchematicPins(const std::string& file) const override;
        CompileStatus chatRun(const Sources& files) override;
        CompileStatus chatLastStatus() const override;
        Sources chatRunning() const override;
        Sources chatPlaying(Assets& samples, CircuitPins* pins = nullptr) const override;
        Sources chatPrevious(Assets& samples) const override;
        Controls chatDials() const override;
        float chatDialValue(int slot) const override;
        void chatSetDial(int slot, float plain) override;
        float chatLoad() const override;
        uint32_t chatAlerts() const override;
        PanelPicture chatPicture(PictureView view, const std::string& name, const PictureState& state) override;
        // For the live tools (ProcessorLive.cpp; any thread): what plays now, the meters measured while a tool listens,
        // the journal of changes, the dials' options' names.
        void chatLive(LiveRead& io) const override;
        void chatListening(bool on) override;
        Changes chatChanges(const std::string& since, bool issue) override;
        std::array<std::vector<std::string>, kMaxControls> chatDialOptions() const override;
        capture::Reply chatCapture(capture::Action action, const capture::Options& options) override;
        double chatHostRate() const override;
        int chatHostBlock() const override;
        std::string chatProject() const override;
        std::string chatAb() const override;
        bool chatSnapshot(Engine::Snapshot& s, std::string& why) override;
        std::string chatPanelFit(const PanelSpec& panel, const Controls& controls) override;
        midilearn::View chatMidi() const override { return learn_.view(); }
        Assets chatAssets() const override { return assetsNow(); }
        bool chatAddAsset(const std::string& path, const std::string& name, std::string& done, std::string& why) override;
        bool chatRemoveAsset(const std::string& name, std::string& why) override;
        bool chatRenameAsset(const std::string& name, const std::string& to, std::string& why) override;
        Assets chatVersionSamples(const projectstore::Project& p, const projectstore::Version& v,
                                  std::vector<std::string>& refused) const override;
        ProjectAnswer chatProjects(const ProjectCall& call) override;   // ProcessorSaves.cpp

        // The samples (above; dsp/Assets.h): the set as it is, under assetsMutex_ (any thread reads it), replaced whole
        // on the message thread and Claude's worker (the Compiler's with it, journalled as `who`'s: `words`).
        mutable std::mutex assetsMutex_;
        Assets assets_;
        mutable std::map<std::string, std::string> base64_;     // each file's base64 by its hash (assetsState: a host
                                                                // may save its state often), the set's only
        std::atomic<uint64_t> assetsRevision_{ 1 };
        Assets assetsNow() const;
        void replaceAssets(Assets next, Journal::Who who, const std::string& words);
        // A decoded sample into the set (in place of one of its name), its caps checked again against the set as it is
        // then: false, with why.
        bool putAsset(const std::shared_ptr<const assets::Asset>& asset, Journal::Who who, std::string& why);
        bool dropAsset(const std::string& name, Journal::Who who, std::string& why);
        // What the session keeps of them (<ASSETS>), and the set a state's or a version's make (each file decoded, but
        // one `reuse` holds already, which it shares; one that cannot be is left out, `refused` saying why; a second of
        // a name in place of the first).
        juce::ValueTree assetsState() const;
        static Assets assetsOfState(const juce::ValueTree& state, std::vector<std::string>& refused);
        static Assets assetsOfVersion(const projectstore::Project& p, const projectstore::Version& v,
                                      const Assets& reuse, std::vector<std::string>& refused);

        // The program: what plays, the editor's draft, the dials. Message thread, and a host's state calls. Declared
        // before the parameters, whose names and texts read controls_ (control()).
        mutable std::mutex programMutex_;
        Sources running_;
        Sources previous_;                                       // what played before running_ (chatPrevious): a run's
        Assets runningAssets_, previousAssets_;                  // the samples each compiled with (CompileStatus::
                                                                 // assets)
        std::vector<std::pair<std::string, std::string>> runningSamples_;   // those running_'s lines read
        CircuitPins runningPins_;                                // the configuration its netlists play with (chatPlaying)
        // Whether `s`, a compile's status, plays another program than running_: its files (a netlist's drawing aside),
        // or the samples its lines read (programMutex_).
        bool changesProgram(const CompileStatus& s) const;
        Draft draft_;
        uint64_t draftRevision_ = 1;
        Controls controls_{};
        PanelSpec panel_;                                        // the running program's (programMutex_)
        uint64_t program_ = 0;                                   // its compile's id (CompileStatus::program)
        // The project open (ProjectStore.h; programMutex_): the one opened or saved last, its version, the preset
        // applied last; the version's files (project().edited compares the draft's) and the dials it or the preset set
        // (project().dialsEdited compares the dials').
        std::string projectName_;
        int projectVersion_ = 0;
        mutable int projectLatest_ = 0;                          // its newest, as last read
        std::string projectPreset_;
        Sources projectFiles_;
        Assets projectAssets_;                                   // its samples, as projectFiles_ (project().edited)
        std::vector<projectstore::Dial> baseDials_;
        // MIDI learn's mappings when a version with none was opened (it left them as they were), else none: saving
        // that project while they are still these is no change of theirs (projectstore::VersionMidi::inherited).
        std::optional<std::vector<midilearn::Mapping>> midiOpened_;
        // How many times a project, a version or none was opened (a version, a fresh start, a session, the open one
        // deleted, A/B's Keep of a version): a save in flight lands as the project open only when none was since,
        // the same version opened again included (landSave).
        uint64_t projectOpens_ = 0;
        // The chats (ChatAgent.h, "The chats"; programMutex_): the project open's id (ProjectStore.h: the session keeps it,
        // to find the project renamed in the Finder since), and this instance's own (its chats while no project is open;
        // kept in the session, <CHAT instance>; no two live processors of this process hold one: Processor.cpp's
        // heldIds). chatOwnerNow: the owner the chat shown belongs to now (the project's id, made when it has none;
        // `opened`: the project read already), projectId_ followed; syncChatOwner tells the agent (open: the owner's
        // newest chat shows; carry: the chat goes along; move, in carry's place, from a project whose file is gone);
        // loadChat: the chat a state's <CHAT> names (setStateInformation), the instance's id with it.
        std::string projectId_, projectIdName_;                   // the id, and the name it was found for
        std::string instanceId_ = chatstore::newId();
        void chatOwnerNow(std::string& owner, std::string& label, const projectstore::Project* opened = nullptr);
        void syncChatOwner(ChatAgent::OwnerChange how, const projectstore::Project* opened = nullptr);
        void loadChat(const juce::ValueTree& chat);
        // The store's Reader (the process's): the files the explorer's poll could not list from the cache, read on its
        // thread (projectsPolled). The listing's entries as the port gives them, projectLatest_ followed.
        std::shared_ptr<projectstore::Reader> reader_ = projectstore::reader();
        // The factory projects' store (the process's, FactoryStore.h): registered with for this processor's life, so the
        // first processor starts it (its thread reads the set on disk) and the last one gone stops and joins its thread,
        // before a host may unload the plugin. Nothing here fetches: that waits for an editor and the user's yes.
        std::shared_ptr<factorystore::Store> factoryStore_ = factorystore::attach();
        std::vector<ProjectEntry> entriesOf(const std::vector<projectstore::Entry>& list) const;
        // The presets in turn (EditorPort::presetCycle; the host's programs; MIDI's program change): the project open's
        // presets as the store's cache has them (presetsCached: never a file read whole here; one not in the cache
        // goes to reader_), read again by poll() every kPresetsPollMs and at once after this processor changed them
        // or the project open (presetsStale_), and by a state load. `hostCurrent_` is the program the host is told
        // (getCurrentProgram): the preset applied or matched last, kept while the dials match none in the project it
        // was set in (`hostFor_`), so a host that sets the program it saved changes nothing; another project open (or
        // none with presets) drops it, to the "-" past the presets in the VST3 (whose host would take a number it
        // was told for the one playing and never apply it), else to the first. A state load reads its project whole
        // (restoreProjectBase), so the cache has its presets and the host's program is the session's from that call
        // on. A program asked off the message thread (a host's setCurrentProgram) or by MIDI (the audio thread: a
        // program change on channel 1, its count in the top bits, the synth's) is applied by poll().
        static constexpr double kPresetsPollMs = 500.0;
        static constexpr int kVst3Programs = 128;                // a VST3 host's list: fixed when the plugin is made
        mutable std::mutex presetsMutex_;
        std::string heldProject_;
        std::vector<projectstore::PresetSummary> heldPresets_;
        uint64_t heldNames_ = 0;                                 // a hash of the project's and the presets' names
        uint64_t heldRead_ = 0;                                  // reader_'s revision when they were read
        double heldAt_ = -1.0e12;                                // ms (poll's clock)
        std::atomic<bool> presetsStale_{ true };
        std::atomic<int> hostCurrent_{ 0 };
        std::string hostFor_;                                    // the project hostCurrent_ was set in (presetsMutex_)
        int hostTold_ = -1;                                     // the program the host was last told of (poll)
        uint64_t namesTold_ = 0;                                 // ... and the names then
        std::atomic<int> hostProgramAsked_{ -1 };
        std::atomic<uint32_t> midiProgram_{ 0 };                 // (count << 8) | program, written by the audio thread
        uint32_t midiProgramSeen_ = 0;
        void refreshPresets(bool force);
        bool choosePreset(int index, const char* who);           // a program by number (the host's, MIDI's)
        int currentPreset(const std::vector<projectstore::PresetSummary>& list, bool& applied) const;   // any thread
        // How `dials` (a version's, a preset's: by name) stand against the dials now: how many of them the program
        // has, and whether every one of those is where it is (within a ten-thousandth of its range).
        std::pair<int, bool> dialsAgainst(const Controls& cs, const std::array<std::vector<std::string>, kMaxControls>& options,
                                          const std::vector<projectstore::Dial>& dials) const;
        // The project calls as the editor's (the user's) and Claude's projects tool make them: `who` the journal's (none:
        // Claude Desktop's, which it knows), `lead` a version's note's words ("Claude: brighter").
        bool applyPresetAs(const std::string& project, const std::string& preset, std::optional<Journal::Who> who,
                           std::string& why);
        bool saveVersionAs(const std::string& name, const std::string& lead, std::optional<Journal::Who> who,
                           SavedVersion& saved, std::string& why);
        // A save in three parts (saveVersionAs runs them in turn; startSave the second on a thread of its own): what it
        // keeps, taken on the message thread (takeSave); the store's read and write (runSave, any thread); the project
        // open made the one saved (landSave, the message thread). A factory project's copy is saveCopy's, whole.
        struct SaveJob
        {
            std::string name, lead;
            std::optional<Journal::Who> who;
            Sources files;
            int active = 0;
            std::string open;                                    // the project open as it was taken, its version and
            int openVersion = 0;                                 // projectOpens_ then (landSave: none opened since)
            uint64_t opens = 0;
            std::string knownId, knownIdName;                    // projectId_ and projectIdName_ as it was taken
            std::string ownerId;                                 // runSave's: the project saved's id as chatOwnerNow
                                                                 // finds it, looked up when knownId is not it
            std::vector<projectstore::Dial> dials;
            std::vector<projectstore::Answer> answers;
            projectstore::VersionMidi midi;
            Assets samples;
            bool copy = false;                                   // a factory project's copy (saveCopy's arguments)
            std::string copyFrom, copyAs;
            std::optional<projectstore::Saved> result;          // runSave's (none: why)
            std::string why;
        };
        SaveJob takeSave(const std::string& name, const std::string& lead, std::optional<Journal::Who> who);
        void runSave(SaveJob& job);
        bool landSave(SaveJob& job, SavedVersion& saved, std::string& why);
        // The editor's save in flight (EditorPort::startSave), the message thread's: its job (the saver thread reads its
        // inputs and writes its result, then saverDone_), the one asked for meanwhile, and what saving() says.
        std::unique_ptr<SaveJob> saveJob_;
        std::thread saver_;
        std::atomic<bool> saverDone_{ false };
        std::optional<std::string> saveAgain_;
        SaveState saveState_;
        mutable std::mutex saveGateMutex_;                       // setSaveGate (probes): the saver waits while shut
        std::condition_variable saveGateOpen_;
        bool saveGateShut_ = false;
        void endSave(bool ok, const SavedVersion& saved, const std::string& why);
        void pollSave();
        void finishSave();                                       // the destructor's: the gate opened, the saver joined,
                                                                 // it lands, and the one asked for meanwhile saves
        // The export off the message thread (EditorPort::startExport; ProcessorExport.cpp): its job taken on the message
        // thread (no file read there), run on exporter_, which writes its progress and outcome into exportState_ under
        // exportMutex_; poll() joins it once it has ended, and the destructor cancels and joins it.
        struct ExportJob
        {
            std::vector<std::string> names;
            bool package = false, allVersions = false;
            std::string path;
        };
        void runExport(const ExportJob& job);
        void endExport(bool ok, const Exported& out, const std::string& why, bool cancelled);
        void pollExport();
        void finishExport();
        std::thread exporter_;
        mutable std::mutex exportMutex_;
        ExportState exportState_;                                // under exportMutex_
        std::atomic<bool> exportCancel_{ false };
        bool exportGateShut_ = false;                            // under exportMutex_ (setExportGate)
        std::condition_variable exportGateOpen_;
        // Package 05: the git job (ProcessorGit.cpp): one at a time on gitThread_, which writes gitState_ under gitMutex_
        // (and catches whatever its work throws, failing the job in words); poll() joins it once it has ended and opens a
        // pulled version when the project is open with no change (gitOpen_); the destructor cancels and joins it.
        void runGit(const GitJob& job);
        void pollGit();
        void finishGit();
        std::thread gitThread_;
        mutable std::mutex gitMutex_;
        GitState gitState_;                                      // under gitMutex_
        std::pair<std::string, int> gitOpen_;                    // under gitMutex_: a pulled version to open
        std::atomic<bool> gitCancel_{ false };
        std::atomic<uint64_t> gitRevision_{ 1 };
        bool openProjectAs(const std::string& name, int version, std::optional<Journal::Who> who, std::string& why);
        bool savePresetAs(const std::string& preset, std::optional<Journal::Who> who, std::string& why);
        int programCount() const;
        // A version's or a preset's dials, by name, for when its program's compile lands (poll; message thread).
        std::vector<projectstore::Dial> projectDials_;
        uint64_t dialsRequest_ = 0;
        // A fresh start's compile (startAfresh): every dial to its default when it lands (poll; message thread).
        uint64_t defaultsRequest_ = 0;
        // A/B listening (ProcessorAb.cpp; EditorPort::ab), the message thread's: what it shows (`st`), each side's dials (its
        // own, by name: a version's saved ones, Current's as they were, then as the user left them on that side), its
        // circuits' answers (a version's, restored before it compiles), the request that compiled each side last (or that
        // the program it plays answers) and the program that plays it (the trim's key: a refine's when the self-test
        // compiled it again), what played before it began (its files, program and dials: endAb's), the requests it made (a
        // program that lands from any other ends it, once its request is past `seenRequest`, the newest known when it
        // began), the side's compile on its way, the loudness job asked for last and the dials it measured each side at
        // (none while a move waits to be measured), and the dials as the last poll() saw them, since when.
        struct AbRun
        {
            using Measured = std::array<std::map<std::string, float>, 2>;
            AbState st;
            std::array<std::vector<projectstore::Dial>, 2> dials, saved;   // as they play; a version's as saved
            std::array<std::vector<projectstore::Answer>, 2> answers;
            std::array<int, 2> active{};                         // each side's tab shown
            Sources currentFiles;                                // Current: the draft when it began, its tab, its dials
            int currentActive = 0;
            std::vector<projectstore::Dial> currentDials;
            int latest = 0;                                      // the project's newest version then
            std::array<uint64_t, 2> request{}, program{};
            Sources beforeFiles;
            uint64_t beforeProgram = 0;
            std::vector<projectstore::Dial> beforeDials;
            std::vector<uint64_t> requests;
            uint64_t seenRequest = 0;
            uint64_t pendingRequest = 0;
            uint64_t job = 0;
            std::optional<Measured> measuredWith;
            std::array<float, kMaxControls> rawSeen{};
            double movedAt = 0.0;                                // seconds() then
            bool restless = false;                               // the dials moved since the last look
        };
        AbRun ab_;
        uint64_t abJobs_ = 0;
        // Close's or Keep's compile on its way (0: none), its files and dials: until it lands the trims stay (that program
        // untrimmed, the side that plays as it was) and a session saved keeps those files (abKept_).
        uint64_t abEnding_ = 0;
        Sources abEndingFiles_;
        std::vector<projectstore::Dial> abEndingDials_;
        // A side's dials (its own), for when its compile lands (poll), as a version's are; ends with the next request past it.
        std::vector<projectstore::Dial> abDials_;
        uint64_t abDialsRequest_ = 0;
        ab::Matcher abMatcher_;                                  // the loudness (AbMatch.h), on a thread of its own
        mutable std::mutex abLineMutex_;
        std::string abLine_;                                     // the A/B in words, for Claude (chatAb: any thread)
        // What a session saved while the A/B is on (or while its Close or Keep compiles) keeps as the program and dials
        // that play (getStateInformation, any thread; under programMutex_): what played before it began, and the dials
        // then (normalised) while each side has its own; the files Keep or Close compiles until they land.
        struct AbKept
        {
            bool on = false;
            Sources run;
            bool dials = false;
            std::array<float, kMaxControls> norms{};
        };
        AbKept abKept_;
        // A session loaded (setStateInformation, any thread): its compile's request, for poll() to end the A/B with (the
        // message thread's); `abLoading_` meanwhile lets the program it plays be untrimmed at once (trimOutput), and
        // `abQueued_` is the newest request the A/B made, which a session loaded takes back when it still waits.
        std::atomic<uint64_t> abStateLoaded_{ 0 };
        std::atomic<bool> abLoading_{ false };
        std::atomic<uint64_t> abQueued_{ 0 };
        void abLanded(const CompileStatus& s);                   // poll(): a compile landed
        void abStopped(const CompileStatus& s, int side);        // abLanded: that side's program stopped as it played
        void abMatched();                                        // poll(): the loudness measured, when it is
        void abSessionLoaded();                                  // poll() and each A/B call: a session loaded ends it
        void abFollowDials();                                    // poll(): dials moved and at rest measured again
        AbRun::Measured abMeasureDials() const;                  // each side's dials as a measure now would take them
        void abMeasure();                                        // the loudness asked for again (both sides' as they play)
        void abPublish();                                        // the trims to the audio thread, the line for Claude
        // It ends: nothing more measured; the trims away, or, while `ending` (Close's or Keep's compile) is on its way,
        // kept until it lands (abLanded).
        void abStop(uint64_t ending = 0);
        void abPlay(int side, bool journal);
        bool abSide(const projectstore::Project* p, int version, int k, std::string& why);   // side k read
        void setDialsToDefaults(const Controls& cs);
        std::vector<projectstore::Dial> dialsNow() const;        // the program's dials, by name, in its own units
        void setDialsByName(const Controls& cs, const std::array<std::vector<std::string>, kMaxControls>& options,
                            const std::vector<projectstore::Dial>& dials);
        // `how`: the chat's change of owner (open: the project's own chats show; the plugin's own chat's open, carry:
        // the conversation goes on with the project it asked for, as a save's does).
        bool openVersion(const projectstore::Project& p, const projectstore::Version& v, std::string& why,
                         ChatAgent::OwnerChange how = ChatAgent::OwnerChange::open);
        // A state load's: the project's file read whole (the store's cache then has its presets), and the dials its
        // version and the preset applied set kept as baseDials_ (what dialsEdited and the presets' `edited` compare).
        void restoreProjectBase();
        bool saveCopy(const std::string& from, const std::string& as, const Sources& files, int active,
                      const std::vector<projectstore::Dial>& dials, const std::vector<projectstore::Answer>& answers,
                      const std::vector<midilearn::Mapping>& midi, const Assets& samples, const std::string& lead,
                      std::optional<Journal::Who> who, SavedVersion& saved,
                      std::string& why);                         // a factory project's: the user's own copy
        // Claude's projects tool (ProcessorSaves.cpp; ChatHost::chatProjects). Its saves, opens and presets run on the
        // message thread, as the editor's PROJECTS runs them: the caller's worker hands a job to poll() (after the dials
        // Claude set, so a save keeps them) and waits for it (onMessageThread: at once on the message thread itself; a
        // job poll() has not taken in kClaudeJobSeconds, or by the chat's stop, is withdrawn, nothing done: the message
        // thread may be the one joining the turn). A list reads the store on the caller's thread (never a file read
        // whole on the message thread), the project open's state from a job. What it did goes to the editor's status
        // (claudeNotice_, which EditorPort::claudeNotice gives); unsavedWhat: what ProjectState::changed counts, in
        // words ("the editor's files", "the samples" or both).
        static constexpr int kClaudeJobSeconds = 10;
        int claudeJobMs_ = kClaudeJobSeconds * 1000;             // (setClaudeJobWaitForTests)
        mutable std::mutex claudeJobMutex_;
        std::condition_variable claudeJobDone_;
        std::function<void()> claudeJob_;                        // asked, not taken yet
        bool claudeJobRunning_ = false;
        uint64_t claudeJobsDone_ = 0;
        bool onMessageThread(std::function<void()> job);         // false: not taken in time, or stopped first
        void runClaudeJob();                                     // poll()
        std::string unsavedWhat() const;
        mutable std::mutex claudeNoticeMutex_;
        ClaudeNotice claudeNotice_;
        void noticeClaude(const std::string& text);              // the editor's status says it
        ProjectAnswer claudeProjectAction(const ProjectCall& call, uint64_t& compile);   // the message thread's part
        std::string claudeProjectList(const ProjectCall& call);                          // the caller's

        juce::AudioProcessorValueTreeState apvts_;
        // The panel's own dials' normalised values (Controls.h's isPanelDial: slot kHostControls + k's is k's), as a
        // host parameter's atomic holds its value: written on the message thread (writeDial, their ports, a state load),
        // read by the audio thread (readParameters) and any reader of raw_.
        std::array<std::atomic<float>, kPanelControls> panelValues_{};
        std::array<std::atomic<float>*, kMaxControls> raw_{};   // the normalised values the audio thread reads
        std::array<juce::RangedAudioParameter*, kHostControls> params_{};
        // Slot c's value written (message thread): a host parameter's as a gesture, the host told; a panel dial's into
        // panelValues_, the host told its state changed (no parameter moved). Whether it moved.
        bool writeDial(int c, float norm);
        float dialDefault01(int c) const;                        // an unused slot's value (a new instance's)
        // The values' meaning (the dials' carry, apply()): the program whose controls the host's values are in, its
        // controls and options, the carries made (each a ticket) and the last one written. The audio thread and
        // chatDialValue read meaningProgram_ before the values (release after apply's writes); the rest under
        // meaningMutex_, which apply holds while it writes and the carrier while it reads (never calling the Compiler
        // under it: a refine asks under the Compiler's mutex).
        mutable std::mutex meaningMutex_;
        Controls meaningControls_{};
        std::array<std::vector<std::string>, kMaxControls> meaningOptions_{};
        uint64_t carryTickets_ = 0;
        uint64_t carryWritten_ = 0;
        std::atomic<uint64_t> meaningProgram_{ 0 };
        // Declared after apvts_, so they are destroyed before the parameters they refer to.
        std::vector<std::unique_ptr<GesturePort>> portStore_;   // JuceParamPorts that say when the editor moved them
        std::vector<funkgui::ParamPort*> ports_;

        Engine engine_;
        Compiler compiler_{ engine_ };                           // after engine_: stops before the Engine goes
        // What the host plays, recorded for Claude (Capture.h; the audio thread writes it in hostBlock and play).
        capture::Recorder capture_;
        mutable capture::Glance glanced_;                // the recorder as recording() saw it last (the message thread's)
        // Dial values Claude set (slot, plain), applied by poll() on the message thread as host gestures.
        mutable std::mutex dialMutex_;
        // Claude's view_panel: a picture asked for (the agent's thread waits), made by poll() on the message thread.
        mutable std::mutex pictureMutex_;
        std::condition_variable pictureDone_;
        bool pictureAsked_ = false, pictureReady_ = false;
        PictureView pictureView_ = PictureView::panel;
        std::string picturePage_;                                // its page, or its netlist (a schematic's)
        PictureState pictureState_;                              // what it draws in place of what plays
        PanelPicture picture_;
        std::vector<std::pair<int, float>> pendingDials_;
        // ---- Package 04: panel previews (ProcessorPreviews.cpp) ---------------------------------------------------------
        struct PreviewEntry
        {
            std::string project;
            int theme = 0;
            uint64_t content = 0;                                // the version's files and dials, hashed
            PreviewState state;
            std::shared_ptr<const preview::Prepared> prepared;   // set by previewThread_; drawn by poll()
            bool queued = false;                                 // waits for previewThread_
            uint64_t used = 0;                                   // previewUse_ when last asked for
            std::shared_ptr<const preview::Request> request;     // until prepared
        };
        void pollPreviews(bool pictureDrawn);                    // poll()'s: one drawn at most, the hold let go
        void finishPreviews();                                   // the destructor's: previewThread_ stopped and joined
        void previewLoop();                                      // previewThread_'s
        mutable std::mutex previewMutex_;
        std::condition_variable previewWake_;
        std::vector<PreviewEntry> previews_;                     // previewMutex_
        bool previewQuit_ = false;                               // previewMutex_
        uint64_t previewUse_ = 0;                                // previewMutex_
        double previewAskedAt_ = 0.0;                            // the message thread's: the last previewPolled (ms)
        std::thread previewThread_;                              // started by the first preview asked for
        ShaderTools previewTools_;                               // the Compiler's (the constructor's)
        std::atomic<uint64_t> previewsDrawn_{ 0 };

        uint64_t seen_ = 0;                                      // the compiler's revision poll() last applied
        uint64_t journaled_ = 0;                                 // the editor's RUN poll() last journalled
        uint64_t stopJournaled_ = 0;                             // the program whose stop poll() last journalled
        std::atomic<uint64_t> chatStopJournaled_{ 0 };           // the program whose stop chatRun journalled
        std::atomic<uint32_t> alerts_{ 0 };
        std::atomic<int> editorW_{ 0 }, editorH_{ 0 };
        // The panel's fit (chatPanelFit, EditorPort.h's panelFitReady): the font made (on the message thread, poll()'s
        // once a program with a .gui file compiled, or a fit asked there), and this Mac's chat and explorer widths as
        // poll() read them.
        std::atomic<bool> fitFonts_{ false };
        std::atomic<int> fitChatWidth_{ 0 }, fitExplorerWidth_{ 0 };
        void readyFit();                                         // message thread
        std::atomic<bool> panelView_{ false };                       // the editor's PANEL view, in the session
        std::atomic<bool> codePanelHidden_{ false };                 // its CODE view's panel hidden under the code,
        std::atomic<bool> guiPanelHidden_{ false };                  // ... under the interface's files, in the session
        std::atomic<bool> chatOpen_{ true };                         // its chat's column open, in the session
        std::atomic<bool> explorerOpen_{ true };                     // its explorer open, in the session
        std::atomic<int> explorerW_{ 0 }, chatW_{ 0 };               // the widths its user set (0: none), in the session
        CodeWindow codeWindow_;                                  // its code window (codeWindow), in the session
                                                                 // (under pagesMutex_)
        mutable std::mutex pagesMutex_;
        std::vector<std::string> guiPages_;                      // its interface's pages (guiPages), in the session
        std::vector<FileView> fileViews_;                        // its netlists' views (fileViews), in the session
                                                                 // (under pagesMutex_)
        // The netlists' session pins (schematicPins: *@ part, input and output lines by file), in memory only, under a
        // mutex of their own (the agent's worker reads them while a Panel sets them).
        mutable std::mutex pinsMutex_;
        std::map<std::string, std::string> schematicPins_;
        WindowHooks windowHooks_;                                // the Standalone's window (message thread)
        uint64_t settingsAsked_ = 0;                             // showSettings()'s count (message thread)
        // Settings (ProcessorDebug.cpp; message thread): the page the app's latest request names, the Debug pages open,
        // and how many times debugRows() gathered (the probes: nothing while none is open).
        std::string settingsPage_;
        int debugViewers_ = 0;
        mutable uint64_t debugGathers_ = 0;
        int irViewers_ = 0;                                      // the editors' IR views open (message thread)
        std::atomic<int> codeWindows_{ 0 };                      // the editors whose code is in a code window now
                                                                 // (set on the message thread, read by chatPanelFit
                                                                 // on any; never in the session)
        // The probes' owners (EditorPort::addProbeOwner; message thread): each one's set, the oldest first. The newest
        // whose set is armed reaches the Engine (sendProbes), none: an empty set.
        struct ProbeOwner
        {
            int id = 0;
            ProbeSet set;
        };
        std::vector<ProbeOwner> probeOwners_;
        int probeOwnersMade_ = 0;
        ProbeSet probesSent_{};
        bool probesSentOnce_ = false;
        void sendProbes();
        double alertsAt_ = 0.0;                                  // when alerts_ last gained a bit (ms)

        std::array<float, kMaxControls> values_{};               // audio thread: the last good normalised values
        std::array<Event, kMaxEventsPerBlock> events_{};
        // The panel's keyboard's notes (playNote): one writer (the message thread), one reader (the audio thread, at
        // the start of a block). A ring of kUiNotes; the writer drops a note when it is full.
        static constexpr uint32_t kUiNotes = 64;
        std::array<Event, kUiNotes> uiNotes_{};
        std::atomic<uint32_t> uiNoteHead_{ 0 }, uiNoteTail_{ 0 };   // written: head by the writer, tail by the reader
        void pushUiEvent(const Event& e);                         // into that ring (the writer's side)
        // MIDI learn (MidiLearn.h): the mappings, learning and the control changes noted (the audio thread's side, in
        // hostBlock); the slots in a gesture of MIDI's (pollMidi's, message thread), and its timer, which runs while
        // there is anything to listen for or a gesture to end (poll() starts it, pollMidi stops it).
        midilearn::Learn learn_;
        uint64_t midiGestures_ = 0;
        void midiControls(const juce::MidiBuffer& midi) noexcept;   // the audio thread: control changes to learn_
        void endMidiGestures();
        void syncWheel();                                        // learn_.setWheel from the Standalone's devices
        bool wheelKnown_ = false;                                // ... read at their serial wheelSerial_
        uint64_t wheelSerial_ = 0;
        struct MidiTimer final : juce::Timer
        {
            explicit MidiTimer(Processor& p) : owner(p) {}
            void timerCallback() override { owner.pollMidi(juce::Time::getMillisecondCounterHiRes()); }
            Processor& owner;
        };
        MidiTimer midiTimer_{ *this };
        static constexpr int kMidiHz = 60;
        bool prepared_ = false;
        int maxBlock_ = 1;
        int largestCall_ = 0;

        // A/B listening's matched loudness (ProcessorAb.cpp): the programs it knows and the gain each plays at (each
        // side's, and while Close or Keep compiles, the program on its way at 1; 0: none), and whether a program it does
        // not know keeps the gain that plays (`hold`: while it is on or ending, so a side's refine, a side's old program
        // while its next compiles and a program from elsewhere until poll() ends it are never turned up). The message
        // thread writes them inside a sequence count (odd while it writes); the audio thread keeps the gain it aims at
        // when the count moved under its read. Then, the audio thread's, the gain as it ramps from one to the next over
        // 20 ms (smootherstep, one tick per sample) when the program that plays has another. The Engine's output only
        // (after the recorder, before the bypass), so the meters, the scopes and a recording are untrimmed.
        struct AbTrims
        {
            static constexpr std::size_t kPrograms = 3;
            std::atomic<uint32_t> sequence{ 0 };
            std::array<std::atomic<uint64_t>, kPrograms> program{};
            std::array<std::atomic<float>, kPrograms> gain{};
            std::atomic<bool> hold{ false };
        };
        AbTrims abTrims_;
        fsdsp::SmoothRamp trimFade_{};
        float trimFrom_ = 1.0f, trimTo_ = 1.0f, trimNow_ = 1.0f;
        bool trimSet_ = false;
        // A switch never plays above the matched level: a program coming that plays lower waits (Engine::holdPending)
        // while the one that plays is turned down to its gain (`gateGain_`), and one that plays higher is turned up
        // only once the program going out is silent (Engine::outgoing).
        float gateGain_ = 1.0f;
        bool gating_ = false;
        // The table read for `program`: false when it moved under the read; else its gain when it is known, and `hold`.
        bool trimLookup(uint64_t program, float& gain, bool& known, bool& hold) const noexcept;
        void trimGate() noexcept;                                 // before each Engine call
        void trimOutput(float* const* channels, int numChannels, int numSamples) noexcept;
        // The bypass: w, the Engine's share of the output, 1 while processing and 0 while bypassed, ramped over 20 ms
        // between them (one tick per sample, so the fade does not depend on how the host splits its blocks).
        fsdsp::SmoothRamp wet_{};
        bool wetSet_ = false;
        std::vector<float> weight_;
        std::vector<float> dry_;                                 // an effect's input per channel (stride maxBlock_)

        // What the live tools read (ProcessorLive.cpp): the host's bypass and blocks, set in hostBlock; and the journal
        // of changes get_status `since` reads (Journal.h), fed by poll() (the dials, the bypass, the transport, the
        // alerts, the editor's runs) and by the calls that change things (the draft, the projects, the chat's tools).
        std::atomic<bool> bypassed_{ false };                    // the host's last block was bypassed
        std::atomic<int> largestHostBlock_{ 0 };                 // since prepareToPlay
        std::atomic<uint64_t> hostBlocks_{ 0 };                  // processBlock's calls
        Journal journal_;
        std::array<std::atomic<float>, kMaxControls> polled_{}; // the dials' normalised values as the journal last saw them
        bool polledBypass_ = false, polledKnown_ = false, polledPlaying_ = false;   // poll()'s (message thread)
        float polledBpm_ = 0.0f;
        double preparedRate_ = 0.0;                              // prepareToPlay's last
        std::atomic<uint64_t> userRun_{ 0 };                     // the request of the editor's last RUN
        void journalPoll();                                      // poll(): the dials moved since, the bypass, the transport
        void syncDials();                                        // the dials as they are now, seen (moves the plugin made)
        std::atomic<uint64_t> ownDials_{ 0 };                    // the slots the plugin set since (slotBit; poll()'s writes)
        void syncOwnDials();                                     // those as they are now, seen: any other move is journalled
        void journalDraft(const Draft& before, const Draft& after, Journal::Who who);
        void journalPrepare(double rate, int block);             // prepareToPlay's settings, when they change
        std::string dialTextOf(int slot, float norm) const;      // slot's value as text, in its units

        // The chat with Claude. Declared after the members its turn calls (ChatHost), destroyed before them.
        ChatAgent chat_{ *this, claude::httpsTransport() };

        // The folder (FolderSync.h), passed by poll() on the message thread: its hooks reach the draft (folderDraftIf:
        // version-guarded, journalled as the user's) and RUN. Declared after what they reach, so it stops (writing out
        // what the plugin changed) before they go. What the session keeps of it is copied under folderMutex_ (a host
        // may save its state on another thread), and a state's <FOLDER> waits there for poll() (folderWanted_).
        bool folderDraftIf(const Draft& now, const Draft& next);
        FolderSync folder_{ FolderSync::Plugin{ [this] { return draft(); },
                                                [this](const Draft& now, const Draft& next) { return folderDraftIf(now, next); },
                                                [this](const Sources& files) { run(files); } } };
        FolderLauncher folderLauncher_;
        struct FolderKept
        {
            bool on = false;
            std::string name;
            bool run = false;
        };
        mutable std::mutex folderMutex_;
        FolderKept folderKept_;                                  // what getStateInformation writes
        std::optional<FolderKept> folderWanted_;                 // a loaded state's, for poll()
        void keepFolder();                                       // folderKept_ from folder_ (message thread)
        uint64_t folderKeptAt_ = 0;                              // the folder's revision keepFolder() last copied
        std::filesystem::path folderRoot() const;                // the projects' folder's Folders/
        static double seconds();                                 // the folder's clock

        // The instance's MCP socket (McpLink.h): Claude Desktop through the chat's tools. Last, so it stops first (a call
        // in flight finishes before the chat goes). The host's name for the track the plugin is on, for list_instances.
        std::unique_ptr<McpLink> mcp_;
        mutable std::mutex trackMutex_;
        std::string trackName_;
        juce::var mcpInfo() const;
    public:
        void updateTrackProperties(const TrackProperties& properties) override;
        const McpLink* mcpLink() const noexcept { return mcp_.get(); }   // probes
    private:

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Processor)
    };

    // The editor factory: exactly one definition per target. Source/editor/gpu/CreateEditorGpu.cpp in the plugin (the
    // FunkGui editor); Source/plugin/CreateEditorGeneric.cpp in the probes (JUCE's generic editor; the probes run the
    // FunkGui Panel headless instead).
    juce::AudioProcessorEditor* createEditor(Processor& processor);

    // The shaderc this binary carries (a .gui file's shaders): in the plug-in (the VST3's and the AU's
    // Contents/Resources: CMakeLists.txt copies the pinned bgfx's there), else beside the Standalone's executable
    // (Windows' shaderc.exe, Linux's shaderc); "" when there is none. `standalone`: the code runs in the Standalone,
    // whose own file on Linux is /proc/self/exe's (JUCE's currentExecutableFile there is argv[0] as typed).
    std::string bundledShaderc(bool standalone);
}
