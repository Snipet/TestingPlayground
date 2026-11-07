// Source/dsp/Compiler.h: compiles programs and hands them to the Engine (JUCE-free).
//
//   request(files)      the editor's RUN: compiled on the compiler's own thread, so the editor never waits for LLVM
//                       (an edit of the panel's files alone is applied to the program that plays: "A panel's edit").
//                       A request made while another compiles replaces any still waiting: the last one wins.
//   compileNow(files)   the same on the calling thread, for the processor's construction and a host's state load,
//                       which want the program playing when they return (and the probes, which want determinism);
//                       after the compile the thread runs, if one does (below, "One compile offers at a time")
//   check(files)        the editor's check while typing: Patch::check on the compiler's thread, after any RUN waiting,
//                       its messages in checkStatus(); nothing is offered, the program that plays plays on. A check
//                       asked while another waits replaces it
//
// The files are the program's (Sources.h): the first plays, and the others are what its #includes read.
//
// A program that compiles is offered to the Engine at once (Engine::offer), compiled for its current rate; one that
// does not leaves the running one playing. Either way the outcome is the status (status(); revision() bumps when one
// arrives): FMinus's messages, the plugin's, the program's dials, how long it took. The thread also destroys, every
// tenth of a second, what the audio thread handed back (Engine::collect), so the processor need not.
//
// Frozen dials (Patch.h). A thread of its own, the freezer, watches the dials (Engine::dials) ten times a second: once
// they have held still for freezeAfter seconds (3 by default) on the program that plays, it compiles that program's
// frozen copy at their values (Patch::freeze, from the recipe the program's compile kept) and offers it to the Engine,
// which plays it if the dials still stand there. Values it has tried are not tried again until a dial moves. It runs
// beside the compiles: a RUN never waits for it.
//
// A program that plays and has a .gui file with shaders gets them built too (fminus/Shaders.h: shaderc, with the
// tools setShaderTools gave; a shader seen before comes from the process's cache, in memory or on disk) and checked by
// the renderer (ShaderTools::check: on macOS Metal's own compile, which the editor's shaders::set() then skips), each on
// a thread of its own, started before FMinus compiles the program and waited for by the thread that compiles (the
// compiler's for a RUN; compileNow's caller's, often the message thread, for the processor's construction and a host's
// state load): their binaries go into the status's panel (PanelShader::binary), and a shader that does not compile, or
// that the renderer refuses, is a note on its own file's line.
//
// The IR (Patch.h, "The IR"; the editor's IR view). While someone looks (setIrWanted: the editor's IR tab is open), a
// RUN keeps the program's LLVM IR, as lowered and as optimised (Patch::compile's keepIr), and the freezer the frozen
// copy's; when someone starts looking while the program that plays was compiled without it, or a compile on the
// calling thread made it (compileNow), the compiler's thread compiles that program again for its IR alone
// (Patch::ir: no code, no instance, nothing offered), after any RUN waiting, and the freezer the frozen copy's that
// plays (Patch::frozenIr). The texts are cut past kIrMaxBytes (at a line's end; IrText says how much there was) and
// handed over in ir() as the compile's other results are, shared, never copied again; with nobody looking nothing is
// printed and the texts held here are let go (a program compiled while someone looked keeps FMinus's own copy in its
// fminus::Program until it is replaced: FMinus holds what it printed). Never on the audio thread, never on the message
// thread.
//
// The self-test (fminus/Circuits.h; SelfTester.h). A netlist that leaves its configuration to FMinus's circuit
// self-test plays a provisional one, and its compile wants the test (Patch::Result::circuitJobs): the jobs of a compile
// that plays (RUN's, compileNow's, a refine's; never a check's, a failed compile's or Measure's) go to this compiler's
// SelfTester, which runs them on threads of its own and stores each answer in circuits::answers() (process-wide), and
// cancels a test the newest program no longer wants. RUN never waits for a test, and a check never asks for one. When
// an answer lands for a netlist the program that plays used provisionally, this thread refines, at a pass of its own
// (never the one whose compile made it due, so a RUN's waiter has that compile's status first): it compiles the files
// of the program that plays (the recipe's, never the draft and never a failed RUN's) again under a new program id, now
// with the answer, and offers it as any program (the effect crossfades; the synth's old voices would fade). It waits
// while a RUN is queued (which reads the store itself: the refine is then dropped), while a recording is armed
// (Engine::ownCode) and, for an instrument, until no voice sounds, fades or is held (Meters), so no held note starts
// again; an instrument's program whose Shared ticks (Patch.h, "Shared on its own": its state moves between notes, and a
// new program's Shared starts afresh) is never refined, its answer playing from the next RUN (which reads the store;
// one of the panel's files alone compiles then too: "A panel's edit"); and it looks again once the compile is done,
// just before it offers: a refine that may not play then is held, and offered at the first pass that may (dropped when
// another program plays first, or when an answer it lacks lands).
// A refine answers no request: it brings the status up to date (CompileStatus::refined), which still answers the
// request before it (`request`, `ok`, and a failed compile's `errors` and `sources`, which the editor and Claude go on
// showing), with the refined program as `program` and, when that request compiled, the refine's notes ending with what
// the self-test chose ("pedal.cir: note: the self-test chose this circuit's configuration: 4x, ADAA, omega(1)").
// A test that gives no answer (nothing under the cost cap finite on every signal, the budget spent, an internal error)
// is not tried again (circuits::Answers::Outcome): this thread then brings the status up to date (its revision the
// next), the netlist's provisional note there replaced by why ("pedal.cir: note: the self-test found no configuration
// for this circuit (...), so the provisional one plays on: 8x IIR, omega(2)"), and the provisional configuration
// plays on; a refine's note of the choice carries the test's own warnings (a block that rings at Nyquist).
// setSelfTesting(false) stops both the tests and the refines (the probes'); selfTestNow() runs the jobs waiting and then
// the refine on the calling thread, as freezeNow does.
//
// One compile offers at a time: compileNow waits for the compile the thread runs (a RUN's, a refine's), and the thread
// starts none while compileNow compiles, so the program offered last is the one compiled last.
//
// A program that stops (Patch.h, "The loop budget", "Bounds checks"; Engine.h). A RUN's program is offered
// `revertible`, and the thread waits for its first blocks before it says how the RUN went: until it has played a block
// with no swap under way, or the audio thread has stopped (no block under way, and none for three of its own, 20 ms at
// least: a host that calls none); a block under way, which a call past its budget makes long, is waited for (as long as
// the largest call's budget takes at a loop of some 256 cycles an iteration, ten seconds at least: firstBlocks). Should
// it stop meanwhile, the Engine has gone back to the program it replaced, and the RUN's outcome is that of a compile
// that failed: `ok` false and `stopped`, `errors` the stop's message ("program.fm:14:10: error: process() ran past its
// budget: a loop that never ends?"; a failed bounds check's at the access, `bounds` set: "program.fm:14:33: error:
// process() went outside an array: index -188 out of bounds for 'line' of type 'float[512]'"), and the program that
// played plays on, its status (`program`, the recipe the freezer and a refine read) as it was. A program that stops
// later (a note, a dial, the input that sets its loop going; one compileNow or a refine offered) is the Engine's to
// fade out; this thread, at its next pass, says so in the status (a revision of its own: `ok` false, `stopped`, the
// stop's message, `sources` the stopped program's files) and plays the program that played before it again: the files
// of the last program of other files that played (compiled again with their configurations, CircuitPins, under a new
// id, with the dials' carry) as a refine is offered; their dials in `controls` and `panel`, their files in `restored`,
// the new id in `program`. With none (the first program, or one a state load brought: no run's, as the processor says),
// nothing plays until the next RUN. A program that stopped is never gone back to again, nor refined, nor frozen: a
// refine built while the program it refines stopped is let go before the offer, and the stop said.
//
// A panel's edit. A RUN (request(files), unless told otherwise) whose files differ from those of the program that plays
// only in its panel's files (Sources.h's isPanelFile: the .gui file, a shader's .sc or .sh) is applied to that program
// without compiling it: nothing is offered, so the program plays on, its instances, its state and its voices untouched
// (no crossfade, no fade: the same samples), and its dials keep their values. The .gui file is read again over what the
// program's compile kept (Patch::readPanel, Recipe::panel), its shaders built as a compile's are, and the status is the
// program's with the new panel: `panelOnly`, `program` the same, `request` the RUN's, `sources` the new files (the
// recipe's too, so a refine, a stop's restore of it and the freezer go on from them), `controls` with the .gui file's
// units and scales, the notes of the panel's files read again, and `carry` the program's (the processor may have seen
// this status and not its compile's: it then writes that carry, once, as it would have); when the displays read other
// fields (a display or a live= mark or a shader's input added, moved or gone), their taps go to the Engine for that
// program (Engine::setTaps), the panel names that table (PanelSpec::tapTable, as each frame does: Meters::tapTable, so
// that a panel reads only the fields of its own table, editor/DisplayItem.h's TapsShown), and the thread waits for the
// audio thread's next block to read them (unless it is idle), so the panel that shows the status reads its own fields
// at once. It is panel-only when all of these hold, and else a compile as any RUN is:
//   - a program plays, as the last status says (it compiled; not stopped; no refine compiled and held), and no
//     self-test answer waits for a RUN to play it (a program whose Shared ticks: below, "The self-test");
//   - the files other than the panel's are the same, in the same order, and at least one panel file changed (an
//     unchanged RUN compiles: it starts the program afresh), none of them one that FMinus read (an #include of it);
//   - both the files that play and the new ones have a .gui file, the same first one (a .gui added, removed or renamed
//     changes where the panel comes from: the #pragma panel lines);
//   - the .gui file leaves each dial's declaration as it plays: its type, range, default and taper (a taper= changes
//     what a value means: the carry's), and an int's or a bool's option names (a value keeps its name across a run);
//   - each sample the program's #pragma sample lines read is the one the samples set now has of its name (none, or the
//     same file: Recipe::samples), so a sample added, replaced or removed since compiles it.
// request(files, false) never applies one (the A/B's sides and its end, a recording's restart, a fresh start, a project
// version opened: each wants the program compiled, as it was saved), nor does compileNow.
//
// The dials' carry (Controls.h's DialCarry). Just before it offers a program (a RUN's, compileNow's unless told not to,
// a refine's), the compiler asks the carrier setDialCarrier gave (the processor's) how the dials come across: what each
// slot's plain value is under the new declarations, kept in its units, and the host's normalised value it was made
// from. The carry goes into the Patch, so the audio thread plays the carried values from the program's first block,
// before the processor has written them to the host's parameters, and into the status, for the processor to write.
// Without a carrier (the probes' own compilers) a Patch carries nothing and the dials read as the host's values say.
#pragma once

#include "dsp/Controls.h"
#include "dsp/Engine.h"
#include "dsp/SelfTester.h"
#include "dsp/Sources.h"
#include "fminus/Patch.h"
#include "fminus/PanelSpec.h"
#include "fminus/Shaders.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace playground
{
    class Engine;

    struct CompileStatus
    {
        uint64_t    revision = 0;              // 0 before the first compile; bumps with each one that finishes
        uint64_t    request = 0;               // the request it answers (request()'s, compileNow()'s)
        bool        ok = false;                // the program compiled and plays
        std::string errors;                    // one message a line: "program.fm:3:12: error: ...", and clang's
                                               // "In file included from program.fm:2:" before one in another file
        std::string notes;                     // the plugin's notes on a program that plays
        double      ms = 0.0;
        Controls    controls{};                // the program's dials, when ok
        PanelSpec   panel;                     // how its #pragma panel lines lay them out, when ok
        Sources     sources;                   // the files compiled
        std::vector<std::string> circuitKeys;  // the self-test's keys of the netlists it generated with a configuration
                                               // to choose (Patch::Result::circuitKeys): the answers its state keeps
        uint64_t    program = 0;               // the program that plays once it is out (Patch::program, Meters::program):
                                               // `request` when it compiled; else the one that plays on (0: none yet),
                                               // or the refine's (above, "The self-test")
        bool        refined = false;           // a refine brought it up to date (above): `program` is the refine's, and
                                               // when `ok`, all but `request`; else the failed compile's outcome stays
        bool        panelOnly = false;         // a panel's edit (above): applied to the program that plays, not compiled
        bool        stopped = false;           // the program of `sources` compiled and stopped as it played (above, "A
                                               // program that stops"): a call ran past its loop budget, or failed a
                                               // bounds check, `errors` say which
        bool        bounds = false;            // then a bounds check failed (Patch.h, "Bounds checks"): an index
                                               // outside its array, `errors` at the access
        uint64_t    stoppedProgram = 0;        // then its id: `request` when it stopped in its first blocks (or in init),
                                               // else the program that played (`program` too when nothing plays now)
        Sources     restored;                  // after such a stop, the files of `program` when this thread played the
                                               // program before again (above): `controls` and `panel` are its then
        CircuitPins circuits;                  // the configuration each netlist of `program` was generated with
                                               // (Patch::Recipe::pins): what a recording's replay compiles it with
        // The circuit fields of `program`'s main module (Patch.h, "Circuits"; Patch::Recipe::bindings): each one's field,
        // module, netlist, extent (its instances' paths), pots and switches, whether the program drives them itself
        // (`own`, and why), and its dials' names and slots in `controls`.
        std::vector<lang::CircuitBinding> circuitDials;
        // The probe table of `program` (Patch.h, "Probes"; Patch::Recipe::probes): each field of its main module of a
        // netlist's module, its elements' paths and the netlist's nodes (name, line, readable, why, dc), what the
        // schematic's probes resolve a net against by name (a ProbeSet's circuit is the index).
        std::vector<circuits::Probed> probes;
        // The samples `program`'s #pragma sample lines read (Patch.h, "Samples"; Patch::Recipe::samples): each one's file
        // and the hash of the one it read ("" for none of that name).
        std::vector<std::pair<std::string, std::string>> samples;
        // The samples `program` compiled with (Patch::Recipe::assets; null: none), which a render of it takes: the
        // processor's set may have changed since.
        Assets      assets;
        // How the dials of `program` came across from the program before it (above, "The dials' carry"), when it
        // compiled; none (no slots) without a carrier, or for a compileNow told not to.
        DialCarry   carry;
    };

    // The last check's outcome (Compiler::check).
    struct CheckStatus
    {
        uint64_t    revision = 0;              // 0 before the first check; bumps with each one that finishes
        std::string errors;                    // as CompileStatus::errors
        double      ms = 0.0;
        Sources     sources;                   // the files checked
    };

    // One of the IR's texts (above, "The IR"), as the view gets it.
    struct IrText
    {
        std::string text;                      // LLVM's text, whole, or cut at a line's end past Compiler::kIrMaxBytes
        std::size_t bytes = 0;                 // the whole text's size and lines
        std::size_t lines = 0;
        std::size_t shownLines = 0;            // the lines `text` holds (lines, when it is whole)
        bool cut() const noexcept { return text.size() < bytes; }
    };

    // What there is of the IR (Compiler::ir). Copies share the texts.
    struct IrStatus
    {
        uint64_t    revision = 0;              // bumps whenever anything below changes
        bool        wanted = false;            // someone looks (Compiler::setIrWanted)
        bool        making = false;            // a compile for the IR alone waits or runs (the program's)
        uint64_t    program = 0;               // the compile the texts are of (CompileStatus::request); 0: none
        std::string file;                      // its program's file ("program.fm"), the IR's source_filename
        std::shared_ptr<const IrText> lowered, optimised;   // null: not kept
        std::string why;                       // why the program's IR could not be made ("" when it was)
        // The frozen copy's (Patch.h, "Frozen dials"): its program, the dials' values it holds, its optimised IR.
        uint64_t    frozenProgram = 0;         // 0: none
        std::array<float, kMaxControls> frozenValues{};
        std::shared_ptr<const IrText> frozen;
    };

    class Compiler
    {
    public:
        explicit Compiler(Engine& engine);
        ~Compiler();                           // stops the thread and waits for it
        Compiler(const Compiler&) = delete;
        Compiler& operator=(const Compiler&) = delete;

        // What building a .gui file's shaders needs (the plugin's shaderc and FunkGui's texts); none until it is set.
        void setShaderTools(ShaderTools tools);
        // The program's samples (dsp/Assets.h; Patch.h, "Samples"), which a RUN and compileNow compile with from now: the
        // set as it is when the compile starts. A refine and a program played again after a stop compile with the ones
        // their program had (Patch::Recipe::assets). Any thread.
        void setAssets(Assets assets);

        // `panelEdits` false: compiled whatever changed (above, "A panel's edit").
        uint64_t request(Sources files, bool panelEdits = true);
        // `carry` false: no carrier asked (a host's state load, whose dials come with it).
        CompileStatus compileNow(Sources files, bool carry = true);
        // Request `id` taken back while it still waits (true): nothing of it compiles, plays or has a status. One the
        // thread compiles already, or another, is left as it is (false). Any thread.
        bool withdraw(uint64_t id);

        // The dials' carry (above): asked for on the thread that offers (the compiler's, compileNow's caller; a refine's
        // under the compiler's mutex, so the carrier never calls back into it). Set it before the first compile.
        using DialCarrier = std::function<DialCarry(const Controls& controls, const PanelSpec& panel)>;
        void setDialCarrier(DialCarrier carrier);

        void check(Sources files);
        CheckStatus checkStatus() const;

        CompileStatus status() const;
        uint64_t revision() const noexcept { return revision_.load(std::memory_order_acquire); }
        bool busy() const;                     // a request waits or compiles
        void wait();                           // until none does

        // Frozen dials (above): on or off, and after how long still. freezeNow() compiles the copy for the dials as they
        // stand now on the calling thread, still or not, and offers it (false when there is nothing to freeze or it
        // does not compile); freezes() counts the copies offered.
        void setFreezing(bool on, double afterSeconds = kFreezeAfterSeconds);
        bool freezeNow();
        int freezes() const noexcept { return freezes_.load(std::memory_order_relaxed); }
        static constexpr double kFreezeAfterSeconds = 3.0;

        // The self-test (above): the tests and the refines on or off (on by default); selfTestNow() runs the jobs
        // waiting, then the refine when one is due or held and may run (no RUN queued, no recording armed, an instrument
        // silent and its Shared not ticking: looked at before it compiles and again before it offers), on the calling
        // thread: true when it offered
        // the refined program; selfTesting() says whether a test waits or runs, or an answer waits to be compiled in or
        // offered; selfTests() counts the tests this compiler's tester started.
        void setSelfTesting(bool on);
        bool selfTestNow();
        bool selfTesting() const;
        int selfTests() const noexcept { return tester_.tests(); }
        // The steps a test may take (SelfTester::setBudget; 0: FMinus's default). Probes only.
        void setSelfTestBudget(std::int64_t steps) noexcept { tester_.setBudget(steps); }

        // The IR (above): whether someone looks (the processor counts the editor's IR views), and what there is.
        // Any thread.
        void setIrWanted(bool on);
        IrStatus ir() const;
        uint64_t irRevision() const noexcept { return irRevision_.load(std::memory_order_acquire); }
        static constexpr std::size_t kIrMaxBytes = 2'000'000;               // a text the view holds, at most (2 MB)
        // An IR's text as the view gets it (IrText): whole, or cut at the end of the line that reaches kIrMaxBytes; null
        // for none.
        static std::shared_ptr<const IrText> irText(const std::string& text);

    private:
        struct Built;                          // a compile not offered yet (Compiler.cpp)
        // `files` compiled at `rate` as program `id` (CompileStatus::request and program, Patch::program: Meters::program
        // names it), not offered; `pins`, the configurations of its netlists (a program played again after a stop).
        static std::unique_ptr<Built> build(float rate, Sources files, uint64_t id, const ShaderTools& tools,
                                            ShaderCache& cache, bool keepIr, const CircuitPins* pins, const Assets& assets);

        void run();
        void freezer();                        // the freezer's thread
        bool freeze(const DialState& d);       // compiles and offers the copy (the recipe under mutex_)
        // A compile's status published (its revision the next; a failed one's `program` the one that plays on), and
        // the status as stored.
        CompileStatus finish(CompileStatus s);
        // A compile the caller ran (compileNow's, a RUN's: its patch offered here when it compiled, with its carry
        // when `carry`), published. `watch` (a RUN's): offered revertible, and its first blocks waited for (above, "A
        // program that stops"); firstBlocks() is the wait, false when the Engine went back from it.
        CompileStatus land(std::unique_ptr<Built> b, uint64_t landed, bool carry = true, bool watch = false);
        bool firstBlocks(uint64_t program);
        // No block under way, and none ended for three of the audio thread's own (20 ms at least): it is stopped.
        bool audioIdle() const;
        // A panel's edit (above): `files` applied as RUN `id` to the program that plays, its status published; false,
        // and nothing done, when they are not one (the caller compiles them).
        bool editPanel(const Sources& files, uint64_t id, const ShaderTools& tools, const Assets& assets);
        // The Engine's stops of the program that plays since last looked at (above): the status, and the program
        // before it played again. Under mutex_ (let go while it compiles).
        void checkStops(std::unique_lock<std::mutex>& lock);
        void carryInto(Built& b, const DialCarrier& carrier);   // the carry asked for and set, before the offer
        // The IR: a program taken (its texts when the compile kept them; else, when someone looks, the IR-only compile
        // due; run() makes it), the frozen copy's. Each takes mutex_ itself and bumps the revision.
        void tookProgram(uint64_t id, const std::string& file, bool kept, const Patch::Ir& ir);
        void frozenIr(const DialState& d);     // the freezer's: the frozen copy that plays, when its text is not held
        void setFrozenIr(const DialState& d, const std::string& text);
        void bumpIr();                         // under mutex_
        // The self-test's (under mutex_): refineDue_ from the answers landed since the program that plays was compiled
        // (a refine held then is stale: handed back, for the caller to let go once mutex_ is); whether a refine may run
        // now, and whether one built may be offered now (no recording armed, an instrument silent and its Shared not
        // ticking); the refine itself, true when it offered (mutex_ held on entry and on return, let go while it
        // compiles and once it offered).
        [[nodiscard]] std::unique_ptr<Built> checkLanded();
        // The outcomes reported since last looked at (circuits::Answers::reported): a test of the program that plays
        // that gave no answer says why in the status (above). Under mutex_.
        void checkOutcomes();
        bool mayRefine() const;
        bool quiet() const;
        // Whether the store holds an answer for a netlist the program that plays used provisionally (under mutex_).
        bool answered() const;
        bool refine(std::unique_lock<std::mutex>& lock);
        // The status brought up to date by a refine that plays (above): its notes end with `chosen`.
        void finishRefine(CompileStatus s, const std::string& chosen);
        // A compile that plays: its recipe, the self-tests it wants and the landed() count read before it began (a
        // refine held is stale then: let go). `previous`: the program that played before is kept (previous_) when its
        // files differ, to play again should this one stop (above); false (a state load's, a program played again after
        // a stop) forgets it.
        void played(Patch::Recipe recipe, std::vector<circuits::Job> jobs, uint64_t landed, bool previous = true);

        Engine& engine_;
        mutable std::mutex mutex_;
        ShaderTools tools_;                    // mutex_
        Assets assets_;                        // mutex_: a RUN's and compileNow's samples (setAssets)
        DialCarrier carrier_;                  // mutex_
        ShaderCache& shaders_ = shaderCache();   // the process's (Shaders.h)
        std::condition_variable wake_, idle_;
        Sources queued_;
        bool hasQueued_ = false;
        bool queuedPanel_ = true;              // the request waiting may be a panel's edit (request's panelEdits)
        bool compiling_ = false;
        bool stop_ = false;
        uint64_t queuedId_ = 0, nextId_ = 0;
        CompileStatus status_;
        std::atomic<uint64_t> revision_{ 0 };
        Sources checkQueued_;                  // the check waiting (mutex_)
        bool hasCheck_ = false;
        CheckStatus check_;
        // The freezer's: the recipe of the program last offered, whether to freeze and after how long, the values
        // tried for which program.
        Patch::Recipe recipe_;                 // mutex_
        // A program that stops (above; mutex_): the program before the one that plays, of other files (0: none), the
        // Engine's stops looked at, and the newest program that stopped (never refined or frozen).
        Patch::Recipe previous_;
        uint64_t stopsSeen_ = 0;
        uint64_t stoppedProgram_ = 0;
        bool freezing_ = true;                 // mutex_
        double freezeAfter_ = kFreezeAfterSeconds;
        uint64_t triedProgram_ = 0;
        std::array<float, kMaxControls> tried_{};
        std::atomic<int> freezes_{ 0 };
        // The IR's (mutex_): someone looks, an IR-only compile due, what there is; the freezer's own: the frozen copy
        // whose text it last compiled (its program and values), so a copy that will not compile is not tried again.
        bool irWanted_ = false;
        bool irDue_ = false;
        IrStatus ir_;
        std::atomic<uint64_t> irRevision_{ 0 };
        uint64_t irFrozenTried_ = 0;
        std::array<float, kMaxControls> irFrozenValues_{};
        // The self-test's (mutex_): the jobs of the compile that plays (its netlists' provisional configurations), the
        // landed() count last looked at, an answer for one of them landed, the tests and refines on.
        std::vector<circuits::Job> playing_;
        uint64_t landedSeen_ = 0;
        uint64_t reportedSeen_ = 0;
        bool refineDue_ = false;
        bool selfTesting_ = true;
        std::unique_ptr<Built> held_;          // a refine compiled, not offered yet (above): mutex_
        SelfTester tester_;                    // its own threads; before thread_, so it outlives the loop
        std::mutex freezeMutex_;               // one freeze at a time (the freezer's, freezeNow's)
        std::condition_variable freezerWake_;
        std::thread thread_;                   // last: started once everything above is
        std::thread freezer_;
    };
}
