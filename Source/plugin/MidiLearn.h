// Source/plugin/MidiLearn.h: MIDI learn for the dials, in plain C++ (no JUCE). The user asks a dial to learn (its menu:
// right-click or ctrl-click, editor/Panel.cpp), moves a control on a MIDI controller, and that control change (its
// number and channel) moves the dial from then on, through the dial's own port as the editor's hand does: a host
// gesture a touch, so a host records it, undoes it and lets its automation take over again as for any touch (the
// last writer wins), and the Engine glides it as any dial (nothing steps).
//
// The instrument's only: an effect takes no MIDI from its host (its plugin type says so), so its dials are learned in
// the host (the host's own menu, which right-click opens there as before).
//
//   a mapping   a dial (by its name, as projects keep dials, so it follows the dial to another slot and waits while
//               the program has no dial of that name) and a control: a channel (1 to 16) and a control number; 14
//               bits when the controller sends a pair (MIDI's control 0 to 31 with its LSB, the number + 32, after
//               it), else 7. One control a dial, one dial a control: learning one takes it from where it was.
//   learning    one dial at a time; the first control change on any channel decides (a control 0 to 31 waits kPairMs
//               for its LSB; an LSB whose MSB came is that pair). The channel mode messages (120 to 127) are refused,
//               and so, while the Standalone's racing wheel is on (setWheel), are its controls on channel 1, the one
//               it sends on (dsp/Events.h's wheelcc: 16 to 24 and 48 to 51; it sends them all again when the devices
//               change, so learning would take the wheel's in place of the knob moved): in words (refusal()), and
//               learning goes on. Elsewhere (a host, the Standalone with no wheel on) those controls are learned as
//               any other, a controller's knob on CC 16 as well: the Engine reads them into the Host block's wheel
//               fields from any channel whether a dial learned them or not.
//   moving      a control's value (7 bits: v / 127; 14 bits: (MSB << 7 | LSB) / 16383, an MSB setting its LSB to 0
//               as MIDI says) is the dial's normalised value. The audio thread only notes the newest value of each
//               slot (controlChange: atomics, no lock, no allocation); the message thread takes them (tick) and
//               writes them through the ports, a touch a gesture: it begins with a value that moves the dial and ends
//               kTouchMs after the last control change.
//
// Threads: controlChange() and listening() on the audio thread; everything else under the class's mutex on any thread
// (the message thread, a host's state calls). The probes call tick() with a clock of their own.
#pragma once

#include "dsp/Controls.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace playground::midilearn
{
    inline constexpr int kChannels = 16;
    inline constexpr int kControls = 128;
    inline constexpr int kPairs = 32;                    // controls 0 to 31 may be the MSB of a pair ...
    inline constexpr int kLsb = 32;                      // ... whose LSB is the number + 32
    inline constexpr int kFirstMode = 120;               // 120 to 127: channel mode messages, never a control
    inline constexpr double kPairMs = 120.0;             // learning waits this long for a control 0 to 31's LSB
    inline constexpr double kTouchMs = 300.0;            // a touch ends this long after its last control change

    struct Mapping
    {
        std::string dial;                                // the dial's name
        int channel = 1;                                 // 1 to 16
        int cc = 0;                                      // 0 to 119 (14 bits: 0 to 31, its LSB cc + 32)
        bool fine = false;                               // 14 bits
        friend bool operator==(const Mapping&, const Mapping&) = default;
    };

    // Whether control `cc` on `channel` (1 to 16) may be learned, `wheel` whether the Standalone's racing wheel is on: ""
    // when it may, else why, in words ("CC 123 is a channel mode message (All Notes Off), not a control"; "CC 17 on
    // channel 1 is the racing wheel's throttle ...").
    std::string refusal(int channel, int cc, bool wheel);
    bool refused(int channel, int cc, bool wheel) noexcept;   // refusal() is not "" (the audio thread's test)
    // In words: "CC 74 on channel 1", "CC 2 and 34 (14-bit) on channel 3".
    std::string describe(const Mapping& m);
    // Each mapping valid (a dial named, a channel 1 to 16, a control 0 to 119, 14 bits only for 0 to 31), at most
    // kMaxControls, one a dial and one a control (a later one wins): what a session, a project and learning keep. A
    // racing wheel's control is a valid mapping: the wheel's refusal is learning's alone, while it is on.
    std::vector<Mapping> tidy(std::vector<Mapping> in);

    // What the editor shows and Claude reads (view()). `words` are by slot, the slots of the program bind() took last
    // (the one apply() took: the editor's); a newer program's controls (a compile apply() has not taken yet, which
    // Claude's tools may read) match `mappings` by name instead (forDial).
    struct View
    {
        std::array<std::string, kMaxControls> words{};   // each slot's control, describe()'s words ("" for none)
        std::vector<Mapping> mappings;                   // every mapping (tidy()'s order)
        int learning = -1;                               // the slot learning, else -1
        std::string learningDial;
        std::string note;                                // learning's last word: a refusal ("" for none)
        std::vector<Mapping> waiting;                    // mappings whose dial the program has none of
        uint64_t revision = 0;

        const Mapping* forDial(std::string_view dial) const noexcept;   // the mapping of the dial so named, else null
    };

    class Learn
    {
    public:
        Learn();
        Learn(const Learn&) = delete;
        Learn& operator=(const Learn&) = delete;

        // ---- the audio thread ----------------------------------------------------------------------------------------
        // A control change: `channel` 0 to 15, `cc` and `value` 0 to 127.
        void controlChange(int channel, int cc, int value) noexcept;
        // Whether a control change can matter (a dial learning, or a mapping bound): else the audio thread skips them.
        bool listening() const noexcept { return listening_.load(std::memory_order_relaxed); }

        // ---- any thread -----------------------------------------------------------------------------------------------
        // The Standalone's racing wheel is on (its controls on channel 1 are not learned meanwhile), and a dial learns.
        void setWheel(bool on) noexcept { wheel_.store(on, std::memory_order_release); }
        bool wheel() const noexcept { return wheel_.load(std::memory_order_acquire); }
        bool learning() const noexcept { return armed_.load(std::memory_order_acquire); }

        // ---- any thread, under the mutex ---------------------------------------------------------------------------
        // The program's dials (their names to slots): the mappings bound to them, a dial learning that the program
        // lost no longer learning, and every touch ended at the next tick.
        void bind(const Controls& controls);
        void setMappings(std::vector<Mapping> mappings);   // in place of those there are (a session's, a version's)
        std::vector<Mapping> mappings() const;
        // Learn for slot's dial (a used one): false when it has none. Learning another dial ends that one's.
        bool arm(int slot);
        void cancel();
        bool forget(int slot);                           // its mapping gone: false when it had none
        int forgetAll();                                 // how many went
        View view() const;
        uint64_t revision() const noexcept { return revision_.load(std::memory_order_acquire); }

        // ---- the message thread ------------------------------------------------------------------------------------
        struct Move
        {
            int slot = 0;
            float value = 0.0f;                          // normalised
        };
        struct Tick
        {
            std::vector<Move> moves;                     // each slot's newest value since the last tick
            uint64_t ended = 0;                          // the slots whose touch ended (slotBit)
            bool learned = false;                        // a dial learned a control now: `mapping`
            Mapping mapping;
            std::string refused;                         // a control refused while learning (its words), now
        };
        // `nowMs`: a clock in ms (the probes' own). Clears `out` first.
        void tick(double nowMs, Tick& out);
        uint64_t touching() const;                       // the slots in a touch (slotBit)

    private:
        // A route: 0 none, else (slot + 1) | kind << 8 (kSeven, kMsb, kLsbKind).
        static constexpr uint16_t kSeven = 0, kMsb = 1, kLsbKind = 2;
        static int index(int channel, int cc) noexcept { return channel * kControls + cc; }
        void rebuild();                                  // routes_ from mappings_ and names_ (mutex held)
        void changed();                                  // a new revision (mutex held)
        void updateListening();                          // (mutex held)

        mutable std::mutex mutex_;
        std::vector<Mapping> mappings_;
        std::array<std::string, kMaxControls> names_{};  // the program's dials' names ("" unused)
        int learning_ = -1;
        std::string learningDial_;
        std::string note_;
        double firstAtMs_ = -1.0;                        // when tick() first saw learning's first control
        std::array<double, kMaxControls> lastMs_{};      // each slot's last move (tick's clock)
        uint64_t touching_ = 0;
        uint64_t endAll_ = 0;                            // touches a bind() ends at the next tick
        std::atomic<uint64_t> revision_{ 1 };

        // The audio thread's side: the routes, learning's capture, each slot's newest value.
        std::array<std::atomic<uint16_t>, kChannels * kControls> routes_{};
        std::array<uint8_t, kChannels * kPairs> msb_{}, lsb_{};       // the audio thread's own
        std::atomic<bool> listening_{ false };
        std::atomic<bool> armed_{ false };
        std::atomic<bool> wheel_{ false };
        std::atomic<uint32_t> first_{ 0 };              // learning's first control: 1 << 16 | channel << 8 | cc
        std::atomic<uint32_t> refusedCc_{ 0 };           // the last control it refused, packed the same way
        std::array<std::atomic<uint64_t>, kChannels * kControls / 64> seen_{};   // the controls seen while learning
        std::array<std::atomic<float>, kMaxControls> values_{};
        std::atomic<uint64_t> fresh_{ 0 };               // the slots whose value moved since the last tick
    };
}
