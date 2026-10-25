// Source/fminus/Patch.h: one program, compiled for the plugin and ready to play (JUCE-free). A program is FMinus source
// whose main module the plugin runs, in the first of its files (Sources.h); the others are what its #includes read,
// by name. What the plugin asks of the main module depends on the plugin's kind (cmake/Product.cmake):
//
//   an effect: module Effect
//       param ...;                                              the dials (Controls.h), at most kMaxControls
//       void init(float rate);                                  optional: when the program starts, and again when the
//                                                               sample rate changes, off the audio thread
//       void process(float* restrict left, float* restrict right, int n);
//                                                               n samples of both channels, in place, on the audio
//                                                               thread (a mono bus: `right` a copy of the left, unheard)
//       void end();                                             optional: before the program goes, off the audio thread
//
//   an instrument: module Voice, one instance a voice (kVoices of them, the Engine assigning notes)
//       param ...;                                              the dials, the same values in every voice
//       void init(float rate);                                  optional, as an effect's
//       void start(float note, float velocity);                 a note-on: the MIDI note number (69 is A 440 Hz) and
//                                                               0..1; the voice may have played another note before
//       void stop();                                            optional: the note-off (the key, or All Notes Off)
//       bool process(float* restrict left, float* restrict right, int n);
//                                                               the voice's next n samples into the buffers, which
//                                                               arrive at 0 (write or add); false once it is silent
//                                                               for good, which frees the voice
//       void end();                                             optional
//
// The methods the audio thread calls (process, start, stop) must not allocate: FMinus refuses a program whose methods
// could reach calloc, malloc or free from them (init and end may: a delay line's memory). They need no
// [[clang::nonblocking]] of their own: the plugin calls each through a function of its own with the type it calls it
// by, appended to the program and marked nonblocking (compile() below), so FMinus checks the method's types and its
// effects, and a method of another type is an error, not a crash.
//
// Controls. The main module's first kMaxControls parameters, in declaration order, are the plugin's dials: a float
// is bound as a stream (Program::Binding::stream; the Engine smooths it and passes a buffer while it moves, null
// while it holds still), an int and a bool as knobs. Parameters past kMaxControls, and those of the modules the main
// module holds, keep their fminus::default (they are set, in every instance, before init runs).
//
// Threads: compile() and reset() run off the audio thread (or while it is stopped), the calls marked noexcept on it.
// The audio thread never destroys a Patch; the Engine hands it back (Engine.h).
#pragma once

#include "dsp/Controls.h"
#include "fminus/PanelSpec.h"
#include "dsp/Sources.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fminus
{
    class Program;
}

namespace testingplaygroundsynth
{
    class Patch
    {
    public:
        // An effect plays one instance; an instrument up to kVoices notes at once, with kInstances instances, so a
        // stolen or swapped voice fades out on one instance while the new note starts on another.
        static constexpr int kVoices = 16;
        static constexpr int kInstances = 32;

        struct Result
        {
            std::unique_ptr<Patch> patch;     // null when the program does not compile or does not fit the plugin
            std::string errors;               // FMinus's messages, one a line ("program.fm:3:12: error: ..."), and
                                              // the plugin's (what the program lacks), empty on success
            std::string notes;                // what the plugin wants known though the program plays (one a line)
            double compileMs = 0.0;
        };

        // Compiles `files` (the first the program, each named in the messages as it is called) for this plugin's
        // kind and creates its instances at `sampleRate`: zeroed, their parameters at their defaults, init run.
        static Result compile(const Sources& files, float sampleRate);
        // A program of one file.
        static Result compile(std::string_view source, float sampleRate, std::string_view fileName = kProgramName);

        ~Patch();
        Patch(const Patch&) = delete;
        Patch& operator=(const Patch&) = delete;

        // Off the audio thread, or while it is stopped: every instance as new (end, zeroed, defaults, init).
        void reset(float sampleRate);
        float rate() const noexcept { return rate_; }

        const Controls& controls() const noexcept { return controls_; }
        // The dials' panel its #pragma panel lines ask for (PanelSpec.h), from the program and the files it includes.
        const PanelSpec& panel() const noexcept { return panel_; }
        int instances() const noexcept { return static_cast<int>(instances_.size()); }
        const Sources& sources() const noexcept { return sources_; }

        // ---- the audio thread ------------------------------------------------------------------------------------
        // Control c's value (its plain value, Controls.h) in instance i, or in every instance (i < 0): what a stream
        // reads while its buffer is null, and what a knob reads.
        void setValue(int c, float plain, int i = -1) noexcept;
        // Control c's samples for the next calls (a real control only; nullptr: it holds still at its value).
        void setStream(int c, const float* samples, int i = -1) noexcept;

        // An effect's instance over n samples, in place.
        void process(float* left, float* right, int n) noexcept;

        // An instrument's voice i.
        void start(int i, float note, float velocity) noexcept;
        void stop(int i) noexcept;
        bool render(int i, float* left, float* right, int n) noexcept;

    private:
        struct Shared;                                 // the Program and the function pointers
        Patch();
        void freeInstances() noexcept;
        void setDefaults(void* instance, std::string_view moduleName, int depth) const;

        std::unique_ptr<Shared> s_;
        std::vector<void*> instances_;
        std::vector<bool> initialised_;
        std::size_t instanceSize_ = 0, instanceAlign_ = 16;
        Controls controls_{};
        PanelSpec panel_;
        std::vector<const void*> params_;              // per control: its fminus::Program::Parameter (null: unused)
        Sources sources_;
        float rate_ = 48000.0f;
    };
}
