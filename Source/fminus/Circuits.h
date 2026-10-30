// Source/fminus/Circuits.h: circuit netlists in a program (JUCE-free). Circuits.cpp reaches FMinus's circuit compiler
// through its public header, fminus/Circuit.h (C++17, no LLVM), as Patch.cpp reaches FMinus through fminus/FMinus.h.
//
// A program's file whose name ends in .cir (isCircuitFile, Sources.h) is a SPICE-style netlist: an analog circuit
// (resistors, capacitors, inductors, pots, switches, diodes, OTAs, ideal op amps) that FMinus's circuit compiler turns
// into one FMinus module (fminus::circuit::generate; FMinus's docs/circuits.md). The program reads it as it reads any
// file, `#include "pedal.cir"`, and gets that module: `Pedal pedal;`, `pedal.init(rate)`, `pedal.set(drive, tone)` on
// the 16-sample grid, `y = pedal.tick(x)` a sample.
//
//   Generator   one compile's (Patch::compile, check): the first #include that reaches a netlist generates its module
//               (once a compile; a second #include, and the compile's second pass, read it again), with the answer the
//               store holds for it (Options::chosen) when there is one. It keeps what the generation said: the errors
//               ("pedal.cir:7:1: error: ...", which stop the compile, ahead of FMinus's own messages), the warnings
//               and, for a netlist whose configuration is left to the self-test with no answer stored, the
//               provisional note and a job (the self-test it wants: Job); and the keys of the netlists with a
//               configuration to choose (the answers a saved session keeps). recite() puts FMinus's messages on a
//               generated module ("pedal.cir.fm:120:5: error: ...") at the netlist's line the generated line comes
//               from: "pedal.cir:9:1: error: ... (in the module generated from pedal.cir, line 120)". A Generator made
//               from texts generated already (a recipe's: Patch::Recipe::circuits) serves them and never generates:
//               a frozen copy keeps the program's layout whatever the store holds by then. One given pins (Sources.h's
//               CircuitPins: a compile's, Patch::Recipe::pins) generates each netlist whose key they name with the
//               configuration they say (the answer, or the provisional one), never the store's: a recording's replay.
//   Answers     the self-test's answers (fminus::circuit::Choice::text(), by the netlist's key), process-wide: every
//               instance and every compile reads one store. The newest 64 are kept. The self-test that fills it runs
//               off the compiler's thread (dsp/SelfTester.h, one a Compiler); the in-flight set keeps two instances
//               from testing one key at once; landed() counts the answers stored, so a compiler sees one land and
//               compiles the program that plays again with it. A saved session and a project's version keep the
//               answers for their netlists (keysOf, restore), so a program loaded plays its refined configuration
//               from its first block.
//
// Thread-safe: answers() under its own mutex; a Generator is one compile's, on the thread that compiles (never the
// audio thread: generating allocates).
#pragma once

#include "dsp/Sources.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace playground::circuits
{
    // A self-test a compile wants: a netlist whose configuration is open (it leaves .oversample, .adaa or .solver to
    // the self-test) with no answer stored, so that the provisional configuration plays until one lands.
    struct Job
    {
        std::string key;                         // its fminus::circuit::selfTestKey: 16 hex digits
        std::string file;                        // the netlist's file ("pedal.cir")
        std::string netlist;                     // its text, as generated

        friend bool operator==(const Job&, const Job&) = default;
    };

    class Answers
    {
    public:
        static constexpr std::size_t kCapacity = 64;   // answers kept; storing one more drops the oldest stored

        // The answer stored for `key` (Choice::text()), or nothing.
        std::optional<std::string> find(std::string_view key) const;
        // `text` (Choice::text()) as `key`'s answer, the newest; landed() moves.
        void store(const std::string& key, const std::string& text);
        // The answers stored for `keys`, as (key, text), in their order (a key with none left out, one named twice once).
        std::vector<std::pair<std::string, std::string>> of(const std::vector<std::string>& keys) const;
        // The in-flight set: claim() marks a key being tested and says true, unless it is stored or in flight already
        // (false: test nothing); release() unmarks it, stored or not.
        bool claim(const std::string& key);
        void release(const std::string& key);
        bool inFlight(std::string_view key) const;
        // How many answers have been stored since the process started (a compiler that saw a number refines when it
        // moves).
        uint64_t landed() const noexcept { return landed_.load(std::memory_order_acquire); }
        std::size_t size() const;
        // Probes only: nothing stored, nothing in flight (landed() keeps counting).
        void clearForTests();

    private:
        mutable std::mutex mutex_;
        std::vector<std::pair<std::string, std::string>> entries_;   // oldest first
        std::set<std::string, std::less<>> flight_;
        std::atomic<uint64_t> landed_{ 0 };
    };

    Answers& answers();

    // An answer kept with a saved session or a project (key and Choice::text()), stored in answers() when it reads back
    // as this FMinus's (Choice::parse: its version) and as the answer for that key; false, nothing stored, when not.
    bool restore(const std::string& key, const std::string& text);

    // The self-test's keys of the netlists among `files` (each .cir file's selfTestKey, as a Generator looks its answer
    // up; one that does not parse has none), in their order: the answers a session or a project keeps with them.
    std::vector<std::string> keysOf(const Sources& files);

    // The note a refined compile ends with (Compiler.h): "pedal.cir: note: the self-test chose this circuit's
    // configuration: 4x, ADAA, omega(1)", from the answer `text` (Choice::text()); "" when it does not read back.
    std::string chosenNote(std::string_view file, std::string_view text);

    class Generator
    {
    public:
        // One compile's: generates the netlists of `files` as #includes reach them, with the store's answers (or, for a
        // key `pins` names, with the pin's: its answer, or none).
        explicit Generator(const Sources& files, const CircuitPins* pins = nullptr);
        // Serves `generated` (name and FMinus text, a recipe's) for the netlists of `files`, and generates nothing.
        Generator(const Sources& files, const Sources& generated);

        // The FMinus text an #include of `name` reads: the module generated from the file of that name (now, the
        // first time), or null when `files` has no netlist of that name. Empty when the netlist has errors.
        const std::string* module(std::string_view name);
        // Every netlist of `files` no #include reached, generated for its messages (check(): the squiggles of a tab
        // the program does not include), as if one had: its errors and warnings join the others, its job does not.
        void generateRest();

        // What the netlists generated said, one message a line, in the order generated: their errors (with their
        // notes), their warnings, and the provisional notes ("pedal.cir: note: the self-test chooses this circuit's
        // configuration; provisional: 8x IIR, omega(2)"). The plan's own notes (blocks, rates, latency) are left out.
        const std::string& errors() const noexcept { return errors_; }
        const std::string& warnings() const noexcept { return warnings_; }
        const std::string& notes() const noexcept { return notes_; }
        // The self-tests the compile wants, and the keys of every netlist generated with a configuration to choose
        // (answered or provisional), in the order generated.
        const std::vector<Job>& jobs() const noexcept { return jobs_; }
        const std::vector<std::string>& keys() const noexcept { return keys_; }
        // The configuration each of those was generated with (its key, the answer or ""), in the order generated: what
        // a compile given them generates again (the recipe's, Patch::Recipe::pins).
        const CircuitPins& pins() const noexcept { return used_; }
        // What was generated (each netlist's name and its module's text), in the order generated: the recipe's.
        Sources generated() const;

        // `messages` (FMinus's, one a line) with each line that cites a generated module (".cir.fm:L:C: ...", and
        // "In file included from ...cir.fm:L:") cited at the netlist's line generated line L comes from.
        std::string recite(const std::string& messages) const;

    private:
        struct Module
        {
            std::string name;                    // the netlist's file
            std::string text;                    // the module generated from it
            std::vector<int> lines;              // fminus::circuit::Result::lines: each generated line's netlist line
        };
        Module* find(std::string_view name);
        Module& generate(const SourceFile& netlist, bool job);

        const Sources& files_;
        const CircuitPins* pins_ = nullptr;      // the configurations to generate with, by key (null: the store's)
        bool replay_ = false;                    // serving a recipe's texts: nothing is generated
        std::vector<std::unique_ptr<Module>> modules_;   // stable: module() hands out their texts
        std::string errors_, warnings_, notes_;
        std::vector<Job> jobs_;
        std::vector<std::string> keys_;
        CircuitPins used_;
    };

    // How many netlists every Generator has generated so far (fminus::circuit::generate calls): a probe's counting
    // hook (fminus.circuit RECIPE: a frozen copy's compile generates nothing).
    uint64_t generations() noexcept;
}
