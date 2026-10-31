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
//               from its first block. Beside them, each test's outcome (Outcome): its warnings, and why when it gave
//               no answer; a key given up on is not tested again in this process, and its compiles say why
//               (gaveUpNote) where the provisional note was. An answer stored that its netlist cannot take (a damaged
//               session's or project's: generate() refuses it, or ignores it and plays the provisional configuration)
//               is erased by the first compile that meets it, which generates without it, so the self-test answers
//               the key again.
//
//   Probes      every netlist is generated with FMinus's probes on (Options::probes; FMinus's docs/circuits.md,
//               "Probes"): its module then carries the fields kProbeFields names after its own, through which the
//               Engine watches up to kProbeSlots of its node voltages as it plays (the schematic's live probes), with
//               the same output bits, messages and self-test key as without them. So the program, its frozen copy and
//               every recompile share one layout, and choosing a probe never compiles anything. Each netlist's nodes
//               (Node: every one but ground, in first-mention order, whether a probe can read it and why not) come
//               with its shape; Patch's probe table finds the fields in the main module's instances.
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
    // ---- the probes (above): the same numbers and names as fminus/Circuit.h's, which Circuits.cpp checks ------------
    //   prb_  int: 0 off (tick writes nothing), else armed            the host's (init sets 0)
    //   prs_  int[kProbeSlots]: each slot's node, Node's index, or -1  the host's (init sets -1); a node out of range or
    //                                                                 not readable writes nothing
    //   prn_  int: the samples written while armed, modulo 2^30       tick's: the newest at (prn_ - 1) % kProbeRing
    //   pr_   float[kProbeSlots * kProbeRing]: slot k's ring at [kProbeRing k, kProbeRing (k + 1)), absolute volts
    //   prp_, pra_, prq_  float[kProbeSlots]: each slot's peak |v|, sum of v and sum of v^2 since the host zeroed them
    //   prv_  float[the nodes]: tick's scratch
    // clear() leaves them all, and the guard sums none of them.
    inline constexpr int kProbeSlots = 4;
    inline constexpr int kProbeRing = 1024;
    inline constexpr const char* kProbeFields[] = { "prb_", "prs_", "prn_", "pr_", "prp_", "pra_", "prq_", "prv_" };
    // Whether `name` is one of them (a generated module's fields that are the probes', never the circuit's).
    bool isProbeField(std::string_view name) noexcept;

    // A node of a netlist, as a probe sees it (fminus::circuit::Node): every node but ground, in the order the netlist
    // first mentions them; a probe's slot takes its index.
    struct Node
    {
        std::string name;                        // as first written
        int line = 0, column = 0;                // its first mention, from 1
        int rate = 1;                            // the steps a host sample of the block that computes it
        double dc = 0;                           // its volts at the DC point (the pots and switches at their defaults)
        bool readable = true;                    // the module holds its voltage each host sample: a probe reads it
        std::string why;                         // when not, why ("inside a filter computed as a whole at 1x (block 3)")

        friend bool operator==(const Node&, const Node&) = default;
    };

    // A field of the main module whose type is a netlist's module, as the probes see it (Patch's probe table, in field
    // order: a ProbeSet's circuit is its index; CompileStatus::probes).
    struct Probed
    {
        std::string field;                       // the main module's field ("amp")
        std::string module;                      // the netlist's module ("DiodeTone")
        std::string file;                        // the netlist ("diodetone.cir")
        std::vector<std::string> paths;          // its elements, row-major ("amp[0]", "amp[1]"; a scalar's "amp")
        std::vector<Node> nodes;                 // the netlist's

        friend bool operator==(const Probed&, const Probed&) = default;
    };

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
        // `key`'s answer taken out of the store (a compile found that its netlist cannot take it): the self-test may
        // claim the key again. landed() does not move.
        void erase(std::string_view key);

        // What a test said beyond its answer: the self-test's own warnings (a block that rings at Nyquist), and, when
        // it gave no answer (nothing under the cost cap finite on every signal, the budget spent, an internal error;
        // never a cancel), why. Kept by key beside the answers, the newest kCapacity. A key given up on is not tested
        // again in this process (the test is deterministic: it would give up again): claim() refuses it, and a
        // compile of its netlist wants no job and says gaveUpNote() where the provisional note was.
        struct Outcome
        {
            bool answered = false;   // an answer was stored with it
            std::string warnings;    // the self-test's own warnings, one a line ("pedal.cir:9:1: warning: ...")
            std::string why;         // no answer: why, in the self-test's words ("no candidate under 3500 model ...")
            std::string provisional; // the netlist's provisional note, what played while it was tested
        };
        void report(const std::string& key, Outcome outcome);
        std::optional<Outcome> outcome(std::string_view key) const;
        // Whether `key`'s test gave no answer (an outcome, not answered).
        bool gaveUp(std::string_view key) const;
        // How many outcomes have been reported since the process started (a compiler that saw a number looks at the
        // program that plays again when it moves).
        uint64_t reported() const noexcept { return reported_.load(std::memory_order_acquire); }
        // Probes only: nothing stored, nothing in flight, no outcome (landed() and reported() keep counting).
        void clearForTests();

    private:
        mutable std::mutex mutex_;
        std::vector<std::pair<std::string, std::string>> entries_;   // oldest first
        std::vector<std::pair<std::string, Outcome>> outcomes_;      // oldest first
        std::set<std::string, std::less<>> flight_;
        std::atomic<uint64_t> landed_{ 0 };
        std::atomic<uint64_t> reported_{ 0 };
    };

    Answers& answers();

    // An answer kept with a saved session or a project (key and Choice::text()), stored in answers() when it reads back
    // as this FMinus's (Choice::parse: its version) and as the answer for that key; false, nothing stored, when not.
    bool restore(const std::string& key, const std::string& text);

    // The self-test's keys of the netlists among `files` (each .cir file's selfTestKey, as a Generator looks its answer
    // up; one that does not parse has none), in their order: the answers a session or a project keeps with them.
    std::vector<std::string> keysOf(const Sources& files);

    // The note a refined compile ends with (Compiler.h): "pedal.cir: note: the self-test chose this circuit's
    // configuration: 4x, ADAA, omega(1)", from the answer `text` (Choice::text()), then the test's warnings
    // (Outcome::warnings, when `key` has an outcome); "" when the answer does not read back.
    std::string chosenNote(std::string_view file, std::string_view text, std::string_view key = {});

    // What a compile says, in place of `provisional` (a netlist's provisional note: "pedal.cir: note: the self-test
    // chooses this circuit's configuration; provisional: 8x IIR, omega(2)"), for a netlist whose test gave no answer:
    // "pedal.cir: note: the self-test found no configuration for this circuit (why), so the provisional one plays on:
    // 8x IIR, omega(2)", then the test's warnings.
    std::string gaveUpNote(std::string_view provisional, const Answers::Outcome& outcome);

    // `notes` (a compile's status) with what the tests of `jobs` that gave no answer said (gaveUpNote): in place of
    // each one's provisional note, or after the rest when it has none; unchanged where it says so already.
    std::string withOutcomes(const std::string& notes, const std::vector<Job>& jobs);

    // `job`'s test's outcome, from what fminus::circuit::selfTest returned (SelfTestResult's warnings, notes and
    // errors; `answered`: an answer is stored): the warnings less the netlist's own (which its compiles say anyway),
    // why when it gave no answer (its last note, or its first error), and the netlist's provisional note.
    Answers::Outcome outcomeOf(const Job& job, bool answered, const std::string& warnings, const std::string& notes,
                               const std::string& errors);

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

        // What a netlist generated (`name`, its file) looks like to the program, as FMinus's circuit compiler says
        // (Result's module, pots and switches: set()'s arguments, in order; its nodes): what a compile checks the
        // circuit dials Lang read against (Patch.h, "Circuits") and finds the probes' nodes in. Null when it was not
        // generated, or has errors, or this Generator serves a recipe's texts.
        struct Shape
        {
            std::string module;
            std::vector<std::string> pots, switches;
            std::vector<Node> nodes;             // Result::nodes (above, "Probes")
        };
        const Shape* shape(std::string_view name) const;
        // The shape of the netlist that generated module `module`, and that netlist's name in `file`; null as shape().
        const Shape* shapeOfModule(std::string_view module, std::string* file = nullptr) const;

    private:
        struct Module
        {
            std::string name;                    // the netlist's file
            std::string text;                    // the module generated from it
            std::vector<int> lines;              // fminus::circuit::Result::lines: each generated line's netlist line
            Shape shape;                         // its module, pots and switches (generate()'s)
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
