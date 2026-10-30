// Source/dsp/Compiler.h: compiles programs and hands them to the Engine (JUCE-free).
//
//   request(files)      the editor's RUN: compiled on the compiler's own thread, so the editor never waits for LLVM.
//                       A request made while another compiles replaces any still waiting: the last one wins.
//   compileNow(files)   the same on the calling thread, for the processor's construction and a host's state load,
//                       which want the program playing when they return (and the probes, which want determinism)
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
// tools setShaderTools gave; a shader seen before comes from the cache): their binaries go into the status's panel
// (PanelShader::binary), and a shader that does not compile is a note on its own file's line.
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
#pragma once

#include "dsp/Controls.h"
#include "dsp/Engine.h"
#include "dsp/Sources.h"
#include "fminus/Patch.h"
#include "fminus/PanelSpec.h"
#include "fminus/Shaders.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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

        uint64_t request(Sources files);
        CompileStatus compileNow(Sources files);

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
        void run();
        void freezer();                        // the freezer's thread
        bool freeze(const DialState& d);       // compiles and offers the copy (the recipe under mutex_)
        void finish(CompileStatus s);
        // The IR: a program taken (its texts when the compile kept them; else, when someone looks, the IR-only compile
        // due; run() makes it), the frozen copy's. Each takes mutex_ itself and bumps the revision.
        void tookProgram(uint64_t id, const std::string& file, bool kept, const Patch::Ir& ir);
        void frozenIr(const DialState& d);     // the freezer's: the frozen copy that plays, when its text is not held
        void setFrozenIr(const DialState& d, const std::string& text);
        void bumpIr();                         // under mutex_

        Engine& engine_;
        mutable std::mutex mutex_;
        ShaderTools tools_;                    // mutex_
        ShaderCache shaders_;
        std::condition_variable wake_, idle_;
        Sources queued_;
        bool hasQueued_ = false;
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
        std::mutex freezeMutex_;               // one freeze at a time (the freezer's, freezeNow's)
        std::condition_variable freezerWake_;
        std::thread thread_;                   // last: started once everything above is
        std::thread freezer_;
    };
}
