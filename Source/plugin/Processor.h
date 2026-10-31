// Source/plugin/Processor.h: the JUCE AudioProcessor. It plays an FMinus program that the editor's text field holds
// (README.md, "Writing a program"): the Engine (Source/dsp/Engine.h) runs it, the Compiler (Source/dsp/Compiler.h)
// compiles what the editor runs, and the processor owns the rest:
//   - thirty-two host parameters, P1 to P32, normalised 0..1, which the running program's first 32 parameters take
//     (Controls.h). Each is a ControlParameter: its name and its text are the program's ("2 cutoff", "1200.0"), and a
//     host is told when they change (parameterInfoChanged). One FunkGui ParamPort each, what the editor's dials write
//     through. A run that brings a parameter the program did not have in that slot moves it to its default;
//   - the program: the files of the one that plays, and the editor's draft (its files, a tab each, and the tab it
//     shows), both kept in the session (a <PROGRAM active=".."> beside the parameters, with a <RUN> and a <DRAFT> of
//     <FILE name=".." text=".."/>s). A new instance plays the first of the EXAMPLES (Init); a session loads its program
//     (compiled at once, on the host's thread) and its dials as it saved them. A session of the version before files
//     (<PROGRAM source=".." draft=".."/>) loads as a program of one file;
//   - effect or instrument (the product's KIND, products/<key>.cmake: product::kIsInstrument): an effect has a
//     stereo input and output and processes in place; an instrument has an output only and gets the block's MIDI
//     notes as Events;
//   - the chat with Claude (ChatAgent.h): its conversation is kept in the session (<CHAT json="..."/>), and its tools
//     act on the draft, RUN and the dials through ChatHost, from the agent's thread (the draft under programMutex_,
//     a dial queued for poll()).
//   - the editor's size, as the user left it (the editor is resizable), its view and its interface's pages, kept in the
//     session (<EDITOR w=".." h=".." view="panel" chat="closed"> with a <PAGE title=".."/> a tabs element).
//
// What a host may do, and what the Engine sees of it:
//   - a block larger than prepareToPlay announced: the Engine gets it in parts of at most that size;
//   - a block of 0 samples (JUCE's VST3 wrapper passes them on): an instrument's Engine still gets its events;
//   - more than kMaxEventsPerBlock events in a block: the block is played in parts, cut at the event that does not
//     fit, so no event is lost;
//   - MIDI: note-on (at velocity 0 a note-off), note-off, the controllers All Sound Off (120) and All Notes Off (123),
//     both an allNotesOff event, the mod wheel (1) and the sustain pedal (64), pitch bend and channel pressure; the
//     rest is ignored. The host's transport (tempo, position, signature, playing) goes to the Engine once a block,
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

#include "plugin/Capture.h"
#include "plugin/ChatAgent.h"
#include "plugin/EditorPort.h"
#include "plugin/Journal.h"
#include "plugin/McpLink.h"
#include "plugin/ProjectStore.h"

#include "dsp/Compiler.h"
#include "dsp/Controls.h"
#include "dsp/Engine.h"
#include "dsp/Events.h"

#include "fsdsp/core/Ramp.h"

#include <funkgui/params/JuceParamPort.h>
#include <funkgui/params/ParamPort.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace playground
{
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
        std::span<funkgui::ParamPort* const> ports() const noexcept { return ports_; }   // P1 to P32
        EditorPort& editorPort() noexcept { return *this; }
        ChatAgent& chatAgent() noexcept { return chat_; }        // probes: its transport, its turns
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
        // moves). Message thread.
        void showSettings() noexcept { ++settingsAsked_; }
        Engine& engine() noexcept { return engine_; }
        Compiler& compiler() noexcept { return compiler_; }   // for the probes
        capture::Recorder& recorder() noexcept { return capture_; }   // the capture tool's recorder (the probes)
        // The most samples one Engine::process call got since prepareToPlay (never more than it announced).
        int largestEngineCall() const noexcept { return largestCall_; }
        // The compiler's outcome to the message thread (the timer's call); settle() waits for a run first. It also makes
        // the picture Claude's view_panel asks for, there.
        void poll();
        void settle();

        // Who reads the meters and the scopes (Engine::readers): the audio thread measures them only while someone does.
        // The editor's Panel is a reader while it lives (EditorPort); a tool that listens to the plugin (Claude
        // Desktop's) adds itself while it listens and removes itself after. Any thread.
        void addMeterReader() noexcept override { engine_.addReader(); }
        void removeMeterReader() noexcept override { engine_.removeReader(); }
        int meterReaders() const noexcept override { return engine_.readers(); }

        // Slot c's dial as the program that plays has it (ControlParameter's name and text).
        Control control(int c) const;
        std::vector<std::string> controlOptions(int c) const;   // its values' names (#pragma panel options), maybe none

    private:
        static BusesProperties buses();                          // the kind's buses (product::kIsInstrument)
        juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

        void hostBlock(juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, bool bypassed) noexcept;
        void readParameters() noexcept;                          // the dials' values, to the Engine
        void play(juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, bool bypassed) noexcept;
        void keepDry(float* const* channels, int numChannels, int numSamples) noexcept;
        void bypassMix(float* const* channels, int numChannels, int numSamples) noexcept;
        void runUnprepared(juce::AudioBuffer<float>& buffer) noexcept;

        // poll()'s work: a finished compile's dials (resetting the renamed ones to their defaults when `reset`).
        void apply(const CompileStatus& s, bool reset);
        void timerCallback() override;

        // EditorPort
        Draft draft() const override;
        uint64_t draftRevision() const override;
        void setDraft(const Draft& draft) override;
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
        float load() const override;
        bool frozen() const override;
        uint32_t alerts() const override;
        std::vector<ProjectEntry> projects() const override;
        std::string projectFolder() const override;
        std::string tidyName(const std::string& typed) const override { return projectstore::tidyName(typed); }
        ProjectState project() const override;
        std::vector<VersionEntry> versions(const std::string& project) const override;
        std::vector<PresetEntry> presets(const std::string& project) const override;
        bool saveVersion(const std::string& name, SavedVersion& saved, std::string& why) override;
        bool openProject(const std::string& name, int version, std::string& why) override;
        bool deleteProject(const std::string& name, std::string& why) override;
        bool deleteVersion(const std::string& name, int version, std::string& why) override;
        bool savePreset(const std::string& preset, std::string& why) override;
        bool applyPreset(const std::string& project, const std::string& preset, std::string& why) override;
        bool deletePreset(const std::string& project, const std::string& preset, std::string& why) override;
        ProjectInfo projectInfo(const std::string& project) const override;
        bool setProjectInfo(const std::string& project, const ProjectInfo& info, std::string& why) override;
        bool exportProject(const std::string& project, bool package, bool allVersions, const std::string& path, Exported& out,
                           std::string& why) override;
        bool importProject(const std::string& path, std::string& name, std::string& why) override;
        std::string exportFolder() const override;
        std::string desktopName() const override { return mcp_ != nullptr ? mcp_->name() : std::string(); }
        bool desktopConnected() const override { return mcp_ != nullptr && mcp_->connected(); }
        ChatPort& chat() override { return chat_; }
        bool panelView() const override { return panelView_.load(); }
        void setPanelView(bool on) override { panelView_.store(on); }
        bool chatOpen() const override { return chatOpen_.load(); }
        void setChatOpen(bool open) override { chatOpen_.store(open); }
        bool explorerOpen() const override { return explorerOpen_.load(); }
        void setExplorerOpen(bool open) override { explorerOpen_.store(open); }
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

        // ChatHost: Claude's tools (ChatAgent.h), from the agent's worker thread.
        Draft chatDraft() const override;
        void chatSetDraft(const Draft& draft) override;
        bool chatSetDraftIf(const Draft& draft, const std::string& file, const std::string& expected) override;
        std::string chatSchematicPins(const std::string& file) const override;
        CompileStatus chatRun(const Sources& files) override;
        CompileStatus chatLastStatus() const override;
        Sources chatRunning() const override;
        Controls chatDials() const override;
        float chatDialValue(int slot) const override;
        void chatSetDial(int slot, float plain) override;
        float chatLoad() const override;
        uint32_t chatAlerts() const override;
        PanelPicture chatPicture(PictureView view, const std::string& name) override;
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

        // The program: what plays, the editor's draft, the dials. Message thread, and a host's state calls. Declared
        // before the parameters, whose names and texts read controls_ (control()).
        mutable std::mutex programMutex_;
        Sources running_;
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
        std::vector<projectstore::Dial> baseDials_;
        // A version's or a preset's dials, by name, for when its program's compile lands (poll; message thread).
        std::vector<projectstore::Dial> projectDials_;
        uint64_t dialsRequest_ = 0;
        std::vector<projectstore::Dial> dialsNow() const;        // the program's dials, by name, in its own units
        void setDialsByName(const Controls& cs, const std::vector<projectstore::Dial>& dials);
        bool openVersion(const projectstore::Project& p, const projectstore::Version& v, std::string& why);

        juce::AudioProcessorValueTreeState apvts_;
        std::array<std::atomic<float>*, kMaxControls> raw_{};   // the normalised values the audio thread reads
        std::array<juce::RangedAudioParameter*, kMaxControls> params_{};
        // Declared after apvts_, so they are destroyed before the parameters they refer to.
        std::vector<std::unique_ptr<GesturePort>> portStore_;   // JuceParamPorts that say when the editor moved them
        std::vector<funkgui::ParamPort*> ports_;

        Engine engine_;
        Compiler compiler_{ engine_ };                           // after engine_: stops before the Engine goes
        // What the host plays, recorded for Claude (Capture.h; the audio thread writes it in hostBlock and play).
        capture::Recorder capture_;
        // Dial values Claude set (slot, plain), applied by poll() on the message thread as host gestures.
        mutable std::mutex dialMutex_;
        // Claude's view_panel: a picture asked for (the agent's thread waits), made by poll() on the message thread.
        std::mutex pictureMutex_;
        std::condition_variable pictureDone_;
        bool pictureAsked_ = false, pictureReady_ = false;
        PictureView pictureView_ = PictureView::panel;
        std::string picturePage_;                                // its page, or its netlist (a schematic's)
        PanelPicture picture_;
        std::vector<std::pair<int, float>> pendingDials_;

        uint64_t seen_ = 0;                                      // the compiler's revision poll() last applied
        uint64_t journaled_ = 0;                                 // the editor's RUN poll() last journalled
        std::atomic<uint64_t> editorRun_{ 0 };                   // the request of the last run (the editor's or Claude's)
        std::atomic<uint32_t> alerts_{ 0 };
        std::atomic<int> editorW_{ 0 }, editorH_{ 0 };
        std::atomic<bool> panelView_{ false };                       // the editor's PANEL view, in the session
        std::atomic<bool> chatOpen_{ true };                         // its chat's column open, in the session
        std::atomic<bool> explorerOpen_{ true };                     // its explorer open, in the session
        mutable std::mutex pagesMutex_;
        std::vector<std::string> guiPages_;                      // its interface's pages (guiPages), in the session
        std::vector<FileView> fileViews_;                        // its netlists' views (fileViews), in the session
                                                                 // (under pagesMutex_)
        // The netlists' session pins (schematicPins: *@ part lines by file), in memory only, under a mutex of their own
        // (the agent's worker reads them while a Panel sets them).
        mutable std::mutex pinsMutex_;
        std::map<std::string, std::string> schematicPins_;
        WindowHooks windowHooks_;                                // the Standalone's window (message thread)
        uint64_t settingsAsked_ = 0;                             // showSettings()'s count (message thread)
        int irViewers_ = 0;                                      // the editors' IR views open (message thread)
        double alertsAt_ = 0.0;                                  // when alerts_ last gained a bit (ms)

        std::array<float, kMaxControls> values_{};               // audio thread: the last good normalised values
        std::array<Event, kMaxEventsPerBlock> events_{};
        // The panel's keyboard's notes (playNote): one writer (the message thread), one reader (the audio thread, at
        // the start of a block). A ring of kUiNotes; the writer drops a note when it is full.
        static constexpr uint32_t kUiNotes = 64;
        std::array<Event, kUiNotes> uiNotes_{};
        std::atomic<uint32_t> uiNoteHead_{ 0 }, uiNoteTail_{ 0 };   // written: head by the writer, tail by the reader
        void pushUiEvent(const Event& e);                         // into that ring (the writer's side)
        bool prepared_ = false;
        int maxBlock_ = 1;
        int largestCall_ = 0;

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
        std::atomic<uint32_t> ownDials_{ 0 };                    // the slots the plugin set since (bits; poll()'s writes)
        void syncOwnDials();                                     // those as they are now, seen: any other move is journalled
        void journalDraft(const Draft& before, const Draft& after, Journal::Who who);
        void journalPrepare(double rate, int block);             // prepareToPlay's settings, when they change
        std::string dialTextOf(int slot, float norm) const;      // slot's value as text, in its units

        // The chat with Claude. Declared after the members its turn calls (ChatHost), destroyed before them.
        ChatAgent chat_{ *this, claude::httpsTransport() };

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
}
