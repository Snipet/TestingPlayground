// Source/fminus/Patch.h: one program, compiled for the plugin and ready to play (JUCE-free). A program is FMinus source
// whose main module the plugin runs, in the first of its files (Sources.h); the others are what its #includes read,
// by name. What the plugin asks of the main module depends on the plugin's kind (products/<key>.cmake):
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
//   either kind, optionally: module Shared, made once for the program, which every field of the main module of type
//   Shared* points at: what an instrument's voices share (wavetables, lookup tables, a sample), built once
//       void init(float rate);                                  optional: before the main module's instances' init,
//                                                               off the audio thread (again when the rate changes)
//       void end();                                             optional: after theirs
//   The audio thread reads it through those pointers; it is the plugin's to free, never a voice's.
//
//   either kind, optionally: module Host, from host.fm, the plugin's own file (#include "host.fm"; hostSource()): one
//   instance, which every field of type Host* of the main module (and of Shared) points at, and which the Engine fills
//   before every call of start, stop and process (setHost): the tempo, the song's position at the call's first sample,
//   the time signature, whether the host plays, and an instrument's mod wheel, pitch bend, aftertouch and sustain
//   pedal. A program reads it, never writes it.
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
// Frozen dials. Once the dials hold still, the plugin compiles the program again with each dial bound as a constant at
// its value (FMinus's Program::Binding::frozen; freeze() below): the optimiser folds what the dials compute, a filter's
// coefficients, a level's expf, a branch on a switch, which an instrument's voices otherwise compute in every call.
// FMinus gives a parameter the same slot in an instance whatever its binding, so the frozen copy runs this Patch's own
// instances: the Engine switches between the two at a block's start (useFrozen) without a seam. The copy compiles in
// the program's own math, FMinus's Math::Vector, whose kernels give a float one result wherever it is computed, from a
// frozen parameter or a knob: so the copy plays the program's very bits, in any blocks (dsp.freeze checks), and all it
// changes is the cost. It was in Math::Fast (LLVM's intrinsics, libm a sample at a time where a loop does not
// vectorise) until FMinus c75caab inlined its own kernels: since then fast math saves only a few percent on average
// and makes a loop with state that calls tanhf or sinf slower than the program itself (README "Frozen dials").
// No fast-math flags: no reassociation, NaN and infinity kept as IEEE has them.
//
// Threads: compile(), freeze() and reset() run off the audio thread (or while it is stopped), the calls marked
// noexcept on it. init and end run in flush-to-zero (fsdsp::ScopedFtz), the mode the program is compiled for, on
// whatever thread resets or destroys it.
// The audio thread never destroys a Patch; the Engine hands it back (Engine.h).
#pragma once

#include "dsp/Controls.h"
#include "dsp/Events.h"
#include "fminus/PanelSpec.h"
#include "dsp/Sources.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fminus
{
    class Program;
}

namespace playground
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
        // The editor's check while typing (Compiler::check): what compile() would say of `files`, but faster and
        // without the code or the instances: FMinus's pass over the program as written (-O0, which finds what is
        // wrong in the text) and the plugin's checks that the main module is there with its methods. What only the
        // second pass finds (a method the audio thread calls that could allocate, a method declared otherwise than
        // the plugin calls it) waits for RUN. Off the audio thread; `patch` is always null.
        static Result check(const Sources& files);

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
        // Whether the program has a module Shared (made once, its init run before the main module's).
        bool shared() const noexcept { return shared_ != nullptr; }
        // The compile it came from (the Compiler's request id; 0 when none was given). The Engine publishes it with the
        // fields its displays read (Meters::program), so a panel never shows one program's fields on another's displays.
        uint64_t program() const noexcept { return program_; }
        void setProgram(uint64_t id) noexcept { program_ = id; }

        // ---- frozen dials (above) ------------------------------------------------------------------------------------
        class Frozen;
        // What compiling the frozen copy needs, kept from the compile: copyable, for another thread.
        struct Recipe
        {
            Sources files;
            std::string source;                        // the program with the plugin's calls appended
            std::array<std::string, kMaxControls> names{};   // each control's parameter, qualified ("" for none)
            Controls controls{};
            uint64_t program = 0;                      // Patch::program
            std::size_t size = 0, alignment = 0;       // the main module's, which the copy must keep
            bool start = false, stop = false, render = false, process = false;   // the calls the program has
        };
        Recipe recipe() const;
        // The frozen copy of the recipe's program at the dials' plain values (each used control's, Controls.h): off
        // the audio thread. Null, with why, when it does not compile or its main module's layout is not the program's.
        // `fastMath`: in FMinus's Math::Fast rather than the program's Math::Vector, for dsp.freeze, which measures
        // what fast math would gain (the plugin never asks for it).
        static std::unique_ptr<Frozen> freeze(const Recipe& recipe, const std::array<float, kMaxControls>& plain,
                                              std::string& why, bool fastMath = false);

        // ---- the audio thread ------------------------------------------------------------------------------------
        // The frozen copy's code for the calls that follow (null: the program's own). It must be of this program
        // (Frozen::program) and outlive its use.
        void useFrozen(const Frozen* f) noexcept;
        bool frozen() const noexcept { return active_ != nullptr; }

        // Control c's value (its plain value, Controls.h) in instance i, or in every instance (i < 0): what a stream
        // reads while its buffer is null, and what a knob reads. As fminus::Program::setValue writes it (a relaxed
        // store; an int's truncated, a bool's true unless 0), through the slot compile() looked up: no call, no lookup.
        void setValue(int c, float plain, int i = -1) noexcept;
        // Control c's samples for the next calls (a real control only; nullptr: it holds still at its value).
        void setStream(int c, const float* samples, int i = -1) noexcept;

        // An effect's instance over n samples, in place.
        void process(float* left, float* right, int n) noexcept;

        // The Host block's fields (host.fm), for the calls that follow; nothing when the program has no module Host.
        void setHost(const HostBlock& h) noexcept;
        bool host() const noexcept { return host_ != nullptr; }
        static std::string_view hostSource() noexcept;   // host.fm's text

        // An instrument's voice i.
        void start(int i, float note, float velocity) noexcept;
        void stop(int i) noexcept;
        bool render(int i, float* left, float* right, int n) noexcept;

        // The field display t (PanelDisplay::tap) shows: its value in instance i, as a float (an int's number, a bool's
        // 0 or 1). After a block, on the audio thread.
        int taps() const noexcept { return static_cast<int>(taps_.size()); }
        float tap(int t, int i) const noexcept;

        // ---- the fields, for a tool that watches the program (dsp/Debug.h) ---------------------------------------
        // Offline only (a Session's Engine, Measure.h; never the one that plays), between calls. The main module's
        // scalar fields as paths, in declaration order: "phase", a held module's "filter.ic2", an array's elements (of
        // at most 16) "osc[1].phase", a vector's lanes "v.x" (float8 and poly float: "v.s5"), the module Shared's through
        // the main module's Shared* field "shared->gain" and Host's through its Host* "host->bpm". Parameters (the
        // dials) are left out, and pointers never followed.
        struct Field
        {
            enum class Type : uint8_t { real, integer, boolean };
            enum class Home : uint8_t { instance, shared, host };   // each instance's, or the one Shared's or Host's
            std::string path;
            std::string module, name;                  // the module that declares it and its name there: "Svf", "ic2"
            std::size_t offset = 0;                    // in bytes, from its home's address
            Type type = Type::real;
            Home home = Home::instance;
        };
        const std::vector<Field>& fieldMap() const noexcept { return fields_; }
        // What the map leaves out, a line each ("buffer: float *, a pointer, not followed").
        const std::vector<std::string>& unmapped() const noexcept { return unmapped_; }
        const void* instance(int i) const noexcept;    // null when there is none
        // A field's value as it is (a NaN or an infinity kept, where tap gives 0; an int's number, a bool's 0 or 1), in
        // instance i (a Shared or Host field: its one).
        double read(const Field& f, int i) const noexcept;

    private:
        struct Compiled;                               // the Program and the function pointers
        Patch();
        void endInstances() noexcept;                  // the main module's end in every instance, then Shared's
        void freeInstances() noexcept;
        void setDefaults(void* instance, std::string_view moduleName, int depth) const;
        void mapFields();                              // fields_ and unmapped_, from the compiled program

        const Compiled& code() const noexcept { return active_ != nullptr ? *active_ : *s_; }

        std::unique_ptr<Compiled> s_;
        const Compiled* active_ = nullptr;             // the frozen copy's code in use, or null
        Recipe recipe_;
        std::vector<void*> instances_;
        std::vector<bool> initialised_;
        std::size_t instanceSize_ = 0, instanceAlign_ = 16;
        Controls controls_{};
        PanelSpec panel_;
        struct Tap
        {
            enum class Type : uint8_t { real, integer, boolean };
            std::size_t offset = 0;                    // in an instance, in bytes
            Type type = Type::real;
        };
        std::vector<Tap> taps_;
        std::vector<Field> fields_;
        std::vector<std::string> unmapped_;
        std::vector<const void*> params_;             // per control: its fminus::Program::Parameter (null: unused)
        // Per control, where its parameter's slot is in an instance (fminus::Program::Parameter's offsets: the stream's
        // pointer, then the value) and the value's type; `none` for a control the program does not have.
        struct Slot
        {
            enum class Kind : uint8_t { none, real, integer, boolean };
            std::size_t stream = 0, value = 0;
            Kind kind = Kind::none;
        };
        std::array<Slot, kMaxControls> slots_{};
        static void store(void* instance, const Slot& slot, float plain) noexcept;
        Sources sources_;
        float rate_ = 48000.0f;
        uint64_t program_ = 0;
        void* shared_ = nullptr;                       // the module Shared's one instance, or null
        bool sharedInitialised_ = false;
        std::size_t sharedSize_ = 0, sharedAlign_ = 16;
        std::vector<std::size_t> sharedFields_;        // the main module's fields of type Shared*: their offsets
        // The module Host's one instance (host.fm), the fields that point at it, and where its own fields are.
        struct HostLayout
        {
            static constexpr std::size_t kNone = static_cast<std::size_t>(-1);
            std::size_t bpm = kNone, beat = kNone, barStart = kNone, beatsPerBar = kNone, beatUnit = kNone,
                        playing = kNone, modwheel = kNone, pitchbend = kNone, aftertouch = kNone, sustain = kNone;
            std::size_t boolSize = 1;
        };
        void* host_ = nullptr;
        std::size_t hostSize_ = 0, hostAlign_ = 16;
        std::vector<std::size_t> hostFields_, sharedHostFields_;
        HostLayout hostLayout_;
    };

    inline void Patch::store(void* instance, const Slot& slot, float plain) noexcept
    {
        char* at = static_cast<char*>(instance) + slot.value;
        switch (slot.kind)
        {
            case Slot::Kind::real:
                std::atomic_ref<float>(*static_cast<float*>(static_cast<void*>(at))).store(plain, std::memory_order_relaxed);
                break;
            case Slot::Kind::integer:
            {
                // C's conversion toward 0, clamped to an int's range, NaN as 0: as FMinus's setValue.
                const double v = std::isnan(plain) ? 0.0 : std::fmin(std::fmax(std::trunc(static_cast<double>(plain)),
                                                                                -2147483648.0), 2147483647.0);
                std::atomic_ref<int32_t>(*static_cast<int32_t*>(static_cast<void*>(at))).store(static_cast<int32_t>(v),
                                                                              std::memory_order_relaxed);
                break;
            }
            case Slot::Kind::boolean:
                std::atomic_ref<bool>(*static_cast<bool*>(static_cast<void*>(at))).store(plain != 0.0f, std::memory_order_relaxed);
                break;
            case Slot::Kind::none:
                break;
        }
    }

    inline void Patch::setValue(int c, float plain, int i) noexcept
    {
        if (c < 0 || c >= kMaxControls)
            return;
        const Slot& slot = slots_[static_cast<std::size_t>(c)];
        if (slot.kind == Slot::Kind::none)
            return;
        if (i >= 0)
        {
            if (i < static_cast<int>(instances_.size()))
                store(instances_[static_cast<std::size_t>(i)], slot, plain);
            return;
        }
        for (void* p : instances_)
            store(p, slot, plain);
    }

    inline void Patch::setStream(int c, const float* samples, int i) noexcept
    {
        if (c < 0 || c >= kMaxControls)
            return;
        const Slot& slot = slots_[static_cast<std::size_t>(c)];
        if (slot.kind != Slot::Kind::real)
            return;
        const auto put = [&slot, samples](void* instance) {
            std::memcpy(static_cast<char*>(instance) + slot.stream, &samples, sizeof samples);
        };
        if (i >= 0)
        {
            if (i < static_cast<int>(instances_.size()))
                put(instances_[static_cast<std::size_t>(i)]);
            return;
        }
        for (void* p : instances_)
            put(p);
    }

    // A frozen copy of a program's code (Patch::freeze): the Engine owns it while it plays, and hands it back as it
    // does a Patch.
    class Patch::Frozen
    {
    public:
        ~Frozen();
        Frozen(const Frozen&) = delete;
        Frozen& operator=(const Frozen&) = delete;
        uint64_t program() const noexcept { return program_; }
        const std::array<float, kMaxControls>& values() const noexcept { return values_; }   // plain, a control each
        double compileMs() const noexcept { return ms_; }

    private:
        friend class Patch;
        Frozen();
        std::unique_ptr<Compiled> code_;
        uint64_t program_ = 0;
        std::array<float, kMaxControls> values_{};
        double ms_ = 0.0;
    };
}
