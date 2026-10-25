// Source/plugin/Processor.h: the JUCE AudioProcessor. It plays an FMinus program that the editor's text field holds
// (README.md, "Writing a program"): the Engine (Source/dsp/Engine.h) runs it, the Compiler (Source/dsp/Compiler.h)
// compiles what the editor runs, and the processor owns the rest:
//   - thirty-two host parameters, P1 to P32, normalised 0..1, which the running program's first 32 parameters take
//     (Controls.h). Each is a ControlParameter: its name and its text are the program's ("2 cutoff", "1200.0"), and a
//     host is told when they change (parameterInfoChanged). One FunkGui ParamPort each, what the editor's dials write
//     through. A run that brings a parameter the program did not have in that slot moves it to its default;
//   - the program: the files of the one that plays, and the editor's draft (its files, a tab each, and the tab it
//     shows), both kept in the session (a <PROGRAM active=".."> beside the parameters, with a <RUN> and a <DRAFT> of
//     <FILE name=".." text=".."/>s). A new instance plays the first of the EXAMPLES; a session loads its program
//     (compiled at once, on the host's thread) and its dials as it saved them. A session of the version before files
//     (<PROGRAM source=".." draft=".."/>) loads as a program of one file;
//   - effect or instrument (cmake/Product.cmake, product::kIsInstrument): an effect has a stereo input and output and
//     processes in place; an instrument has an output only and gets the block's MIDI notes as Events;
//   - the chat with Claude (ChatAgent.h): its conversation is kept in the session (<CHAT json="..."/>), and its tools
//     act on the draft, RUN and the dials through ChatHost, from the agent's thread (the draft under programMutex_,
//     a dial queued for poll()).
//   - the editor's size, as the user left it (the editor is resizable), kept in the session (<EDITOR w=".." h=".."/>).
//
// What a host may do, and what the Engine sees of it:
//   - a block larger than prepareToPlay announced: the Engine gets it in parts of at most that size;
//   - a block of 0 samples (JUCE's VST3 wrapper passes them on): an instrument's Engine still gets its events;
//   - more than kMaxEventsPerBlock events in a block: the block is played in parts, cut at the event that does not
//     fit, so no event is lost;
//   - MIDI: note-on (at velocity 0 a note-off), note-off, and the controllers All Sound Off (120) and All Notes Off
//     (123), both an allNotesOff event; everything else is ignored;
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

#include "plugin/ChatAgent.h"
#include "plugin/EditorPort.h"

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
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace testingplaygroundsynth
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
        Engine& engine() noexcept { return engine_; }
        // The most samples one Engine::process call got since prepareToPlay (never more than it announced).
        int largestEngineCall() const noexcept { return largestCall_; }
        // The compiler's outcome to the message thread (the timer's call); settle() waits for a run first.
        void poll();
        void settle();

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
        Sources running() const override;
        Controls controls() const override;
        PanelSpec panel() const override;
        uint64_t program() const override;
        bool meters(Meters& out) const override;
        void scope(bool input, float* out, int n) const override;
        float load() const override;
        uint32_t alerts() const override;
        ChatPort& chat() override { return chat_; }

        // ChatHost: Claude's tools (ChatAgent.h), from the agent's worker thread.
        Draft chatDraft() const override;
        void chatSetDraft(const Draft& draft) override;
        CompileStatus chatRun(const Sources& files) override;
        CompileStatus chatLastStatus() const override;
        Sources chatRunning() const override;
        Controls chatDials() const override;
        float chatDialValue(int slot) const override;
        void chatSetDial(int slot, float plain) override;
        float chatLoad() const override;
        uint32_t chatAlerts() const override;

        // The program: what plays, the editor's draft, the dials. Message thread, and a host's state calls. Declared
        // before the parameters, whose names and texts read controls_ (control()).
        mutable std::mutex programMutex_;
        Sources running_;
        Draft draft_;
        uint64_t draftRevision_ = 1;
        Controls controls_{};
        PanelSpec panel_;                                        // the running program's (programMutex_)
        uint64_t program_ = 0;                                   // its compile (CompileStatus::request)

        juce::AudioProcessorValueTreeState apvts_;
        std::array<std::atomic<float>*, kMaxControls> raw_{};   // the normalised values the audio thread reads
        std::array<juce::RangedAudioParameter*, kMaxControls> params_{};
        // Declared after apvts_, so they are destroyed before the parameters they refer to.
        std::vector<std::unique_ptr<funkgui::JuceParamPort>> portStore_;
        std::vector<funkgui::ParamPort*> ports_;

        Engine engine_;
        Compiler compiler_{ engine_ };                           // after engine_: stops before the Engine goes
        // Dial values Claude set (slot, plain), applied by poll() on the message thread as host gestures.
        std::mutex dialMutex_;
        std::vector<std::pair<int, float>> pendingDials_;

        uint64_t seen_ = 0;                                      // the compiler's revision poll() last applied
        std::atomic<uint64_t> editorRun_{ 0 };                   // the request of the last run (the editor's or Claude's)
        std::atomic<uint32_t> alerts_{ 0 };
        std::atomic<int> editorW_{ 0 }, editorH_{ 0 };
        double alertsAt_ = 0.0;                                  // when alerts_ last gained a bit (ms)

        std::array<float, kMaxControls> values_{};               // audio thread: the last good normalised values
        std::array<Event, kMaxEventsPerBlock> events_{};
        bool prepared_ = false;
        int maxBlock_ = 1;
        int largestCall_ = 0;

        // The bypass: w, the Engine's share of the output, 1 while processing and 0 while bypassed, ramped over 20 ms
        // between them (one tick per sample, so the fade does not depend on how the host splits its blocks).
        fsdsp::SmoothRamp wet_{};
        bool wetSet_ = false;
        std::vector<float> weight_;
        std::vector<float> dry_;                                 // an effect's input per channel (stride maxBlock_)

        // The chat with Claude. Declared last, destroyed first: its turn calls the members above (ChatHost).
        ChatAgent chat_{ *this, claude::httpsTransport() };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Processor)
    };

    // The editor factory: exactly one definition per target. Source/editor/gpu/CreateEditorGpu.cpp in the plugin (the
    // FunkGui editor); Source/plugin/CreateEditorGeneric.cpp in the probes (JUCE's generic editor; the probes run the
    // FunkGui Panel headless instead).
    juce::AudioProcessorEditor* createEditor(Processor& processor);
}
