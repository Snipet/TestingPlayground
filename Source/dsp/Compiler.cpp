// Source/dsp/Compiler.cpp: see Compiler.h.
#include "dsp/Compiler.h"

#include "dsp/Engine.h"
#include "fminus/Circuits.h"
#include "fminus/Patch.h"

#include "Product.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace playground
{
    namespace
    {
        // The panel's shaders built (their binaries in it), each from its file; a failure appended to `notes`.
        void buildShaders(PanelSpec& panel, const Sources& files, const ShaderTools& tools, ShaderCache& cache,
                          std::string& notes)
        {
            for (PanelShader& sh : panel.shaders)
            {
                const auto f = std::find_if(files.begin(), files.end(), [&](const SourceFile& s) { return s.name == sh.file; });
                if (f == files.end())
                    continue;                            // GuiSpec said so
                const ShaderBuild b = cache.build(sh.file, f->text, sh.inputs, tools);
                if (b.ok())
                {
                    sh.binary = b.binary;
                    sh.id = shaderId(sh.binary);
                }
                else if (notes.find(b.error) == std::string::npos)
                    notes += b.error;
            }
        }

        std::string fileOf(const CompileStatus& s)
        {
            return s.sources.empty() ? std::string(kProgramName) : s.sources.front().name;
        }
    }

    // A compile done and not offered yet: its status, its program (null when it did not compile), and what the compiler
    // keeps of it once it plays.
    struct Compiler::Built
    {
        CompileStatus status;
        std::unique_ptr<Patch> patch;
        Patch::Recipe recipe;                        // the freezer's
        Patch::Ir ir;                                // when `keepIr` (Patch::compile's)
        bool keepIr = false;
        std::vector<circuits::Job> jobs;             // the self-tests its netlists want
        uint64_t landed = 0;                         // circuits::answers().landed() before it began (a refine's)
    };

    std::unique_ptr<Compiler::Built> Compiler::build(float rate, Sources files, uint64_t id, const ShaderTools& tools,
                                                     ShaderCache& cache, bool keepIr)
    {
        auto b = std::make_unique<Built>();
        Patch::Result r = Patch::compile(files, rate, keepIr);
        b->ir = std::move(r.ir);
        b->keepIr = keepIr;
        b->jobs = std::move(r.circuitJobs);
        CompileStatus& s = b->status;
        s.request = id;
        s.ok = r.patch != nullptr;
        s.errors = std::move(r.errors);
        s.notes = std::move(r.notes);
        s.ms = r.compileMs;
        s.sources = std::move(files);
        s.circuitKeys = std::move(r.circuitKeys);
        if (r.patch)
        {
            s.controls = r.patch->controls();
            s.panel = r.patch->panel();
            if (!s.panel.shaders.empty())
                buildShaders(s.panel, s.sources, tools, cache, s.notes);
            r.patch->setProgram(id);
            s.program = id;
            b->recipe = r.patch->recipe();
            s.circuits = b->recipe.pins;
            b->patch = std::move(r.patch);
        }
        return b;
    }

    Compiler::Compiler(Engine& engine)
        : engine_(engine), thread_([this] { run(); }), freezer_([this] { freezer(); })
    {
    }

    Compiler::~Compiler()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        freezerWake_.notify_all();
        thread_.join();
        freezer_.join();
    }

    void Compiler::setFreezing(bool on, double afterSeconds)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        freezing_ = on;
        freezeAfter_ = std::max(0.0, afterSeconds);
    }

    bool Compiler::freeze(const DialState& d)
    {
        const std::lock_guard<std::mutex> one(freezeMutex_);
        Patch::Recipe recipe;
        bool keepIr = false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (recipe_.program == 0 || recipe_.program != d.program)
                return false;
            recipe = recipe_;
            keepIr = irWanted_;
        }
        std::string why, ir;
        std::unique_ptr<Patch::Frozen> f = Patch::freeze(recipe, d.plain, why, false, keepIr ? &ir : nullptr);
        if (!f)
            return false;
        if (keepIr)
            setFrozenIr(d, ir);                    // before it can play: the view never shows another copy's
        engine_.offerFrozen(std::move(f));
        freezes_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool Compiler::freezeNow()
    {
        DialState d;
        for (int k = 0; k < 100 && !engine_.dials(d); ++k)
            std::this_thread::yield();
        return d.program != 0 && freeze(d);
    }

    void Compiler::freezer()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_)
        {
            freezerWake_.wait_for(lock, std::chrono::milliseconds(100), [this] { return stop_; });
            if (stop_)
                break;
            const bool on = freezing_;
            const double after = freezeAfter_;
            lock.unlock();
            DialState d;
            const bool read = engine_.dials(d);
            if (engine_.ownCode())
                triedProgram_ = 0;                     // a recording plays the program's own code: after it, try again
            else if (on && read && d.program != 0)
            {
                if (d.stillSeconds < after)
                    triedProgram_ = 0;                 // a dial moved: any values may be tried again
                else if (!d.frozen && !(triedProgram_ == d.program && tried_ == d.plain))
                {
                    triedProgram_ = d.program;
                    tried_ = d.plain;
                    freeze(d);
                }
            }
            if (read && d.frozen && d.program != 0)
                frozenIr(d);                           // someone looks at a copy whose text is not held
            lock.lock();
        }
    }

    // ---- the IR ---------------------------------------------------------------------------------------------------------
    std::shared_ptr<const IrText> Compiler::irText(const std::string& text)
    {
        if (text.empty())
            return nullptr;
        auto t = std::make_shared<IrText>();
        t->bytes = text.size();
        t->lines = static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + (text.back() == '\n' ? 0 : 1);
        if (text.size() <= kIrMaxBytes)
        {
            t->text = text;
            t->shownLines = t->lines;
        }
        else
        {
            const std::size_t end = text.rfind('\n', kIrMaxBytes - 1);
            t->text = text.substr(0, end == std::string::npos ? kIrMaxBytes : end + 1);
            t->shownLines = static_cast<std::size_t>(std::count(t->text.begin(), t->text.end(), '\n'));
        }
        return t;
    }

    void Compiler::bumpIr()
    {
        ir_.revision += 1;
        irRevision_.store(ir_.revision, std::memory_order_release);
    }

    void Compiler::setIrWanted(bool on)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (on == irWanted_)
                return;
            irWanted_ = on;
            if (!on)
            {
                // Nobody looks: nothing held, nothing due.
                irDue_ = false;
                const uint64_t revision = ir_.revision;
                ir_ = IrStatus{};
                ir_.revision = revision;
            }
            else if (recipe_.program != 0 && !(ir_.program == recipe_.program && ir_.optimised))
            {
                irDue_ = true;                         // the program that plays was compiled without it
                ir_.making = true;
            }
            ir_.wanted = on;
            bumpIr();
        }
        wake_.notify_all();
    }

    IrStatus Compiler::ir() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return ir_;
    }

    void Compiler::tookProgram(uint64_t id, const std::string& file, bool kept, const Patch::Ir& ir)
    {
        // The texts as the view holds them, before the lock: a large one takes a while to copy.
        std::shared_ptr<const IrText> lowered = kept ? irText(ir.lowered) : nullptr;
        std::shared_ptr<const IrText> optimised = kept ? irText(ir.optimised) : nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (!irWanted_)
                return;                                // nobody looks: nothing is kept (setIrWanted let it all go)
            ir_.program = id;
            ir_.file = file;
            ir_.why.clear();
            ir_.frozenProgram = 0;                     // a new program: its frozen copy is another
            ir_.frozenValues = {};
            ir_.frozen.reset();
            if (optimised)
            {
                ir_.lowered = std::move(lowered);
                ir_.optimised = std::move(optimised);
                irDue_ = false;
            }
            else
            {
                ir_.lowered.reset();                   // someone started looking since it began: the IR alone, next
                ir_.optimised.reset();
                irDue_ = true;
            }
            ir_.making = irDue_;
            bumpIr();
        }
        wake_.notify_all();
    }

    void Compiler::setFrozenIr(const DialState& d, const std::string& text)
    {
        std::shared_ptr<const IrText> t = irText(text);
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!irWanted_ || recipe_.program != d.program || !t)
            return;
        ir_.frozenProgram = d.program;
        ir_.frozenValues = d.plain;
        ir_.frozen = std::move(t);
        bumpIr();
    }

    void Compiler::frozenIr(const DialState& d)
    {
        if (irFrozenTried_ == d.program && irFrozenValues_ == d.plain)
            return;                                    // it did not compile: not again for this copy
        Patch::Recipe recipe;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (!irWanted_ || recipe_.program != d.program
                || (ir_.frozen && ir_.frozenProgram == d.program && ir_.frozenValues == d.plain))
                return;                                // nobody looks, another program, or held
            recipe = recipe_;
        }
        std::string text, why;
        if (Patch::frozenIr(recipe, d.plain, text, why))
            setFrozenIr(d, text);
        else
        {
            irFrozenTried_ = d.program;
            irFrozenValues_ = d.plain;
        }
    }

    uint64_t Compiler::request(Sources files)
    {
        uint64_t id = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            queued_ = std::move(files);
            hasQueued_ = true;
            id = queuedId_ = ++nextId_;
        }
        wake_.notify_all();
        return id;
    }

    CompileStatus Compiler::compileNow(Sources files)
    {
        uint64_t id = 0;
        ShaderTools tools;
        {
            // One compile offers at a time (above): the thread's (a RUN's, a refine's) ends first, and the thread starts
            // none while this one compiles, so this program, offered last, is the one that plays.
            std::unique_lock<std::mutex> lock(mutex_);
            idle_.wait(lock, [this] { return !compiling_ || stop_; });
            compiling_ = true;
            id = ++nextId_;
            tools = tools_;
        }
        // Never the IR here (the calling thread may be the message thread): someone looking has it made on the
        // compiler's thread (tookProgram).
        const uint64_t landed = circuits::answers().landed();
        CompileStatus s = land(build(engine_.rate(), std::move(files), id, tools, shaders_, false), landed);
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            compiling_ = false;
        }
        idle_.notify_all();
        wake_.notify_all();                        // a RUN that waited for it
        return s;
    }

    CompileStatus Compiler::land(std::unique_ptr<Built> b, uint64_t landed)
    {
        if (b->patch)
        {
            engine_.offer(std::move(b->patch));
            played(std::move(b->recipe), std::move(b->jobs), landed);
            // Its IR before its status: a view that sees the status has the text.
            tookProgram(b->status.request, fileOf(b->status), b->keepIr, b->ir);
        }
        return finish(std::move(b->status));
    }

    CompileStatus Compiler::finish(CompileStatus s)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!s.ok)
        {
            s.program = recipe_.program;           // the program that played plays on
            s.circuits = recipe_.pins;
        }
        s.revision = status_.revision + 1;
        status_ = std::move(s);
        revision_.store(status_.revision, std::memory_order_release);
        return status_;
    }

    void Compiler::setShaderTools(ShaderTools tools)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        tools_ = std::move(tools);
    }

    void Compiler::check(Sources files)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            checkQueued_ = std::move(files);
            hasCheck_ = true;
        }
        wake_.notify_all();
    }

    CheckStatus Compiler::checkStatus() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return check_;
    }

    CompileStatus Compiler::status() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }

    bool Compiler::busy() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return hasQueued_ || compiling_;
    }

    void Compiler::wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [this] { return (!hasQueued_ && !compiling_) || stop_; });
    }

    void Compiler::run()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_)
        {
            // A RUN waits while compileNow (or selfTestNow's refine) compiles on another thread.
            wake_.wait_for(lock, std::chrono::milliseconds(100),
                           [this] { return (hasQueued_ && !compiling_) || hasCheck_ || irDue_ || stop_; });
            if (stop_)
                break;
            bool compiled = false;
            if (hasQueued_ && !compiling_)
            {
                Sources files = std::move(queued_);
                const uint64_t id = queuedId_;
                const ShaderTools tools = tools_;
                const bool keepIr = irWanted_;         // someone looks: this compile keeps the program's IR
                hasQueued_ = false;
                compiling_ = true;
                lock.unlock();
                const uint64_t landed = circuits::answers().landed();
                land(build(engine_.rate(), std::move(files), id, tools, shaders_, keepIr), landed);
                lock.lock();
                compiling_ = false;
                compiled = true;
                idle_.notify_all();                    // wait() (when no other RUN waits) and compileNow
            }
            if (hasCheck_ && !hasQueued_ && !stop_)
            {
                // The editor's check, once no RUN waits: its messages only.
                Sources files = std::move(checkQueued_);
                hasCheck_ = false;
                lock.unlock();
                const Patch::Result r = Patch::check(files);
                lock.lock();
                check_.revision += 1;
                check_.errors = r.errors;
                check_.ms = r.compileMs;
                check_.sources = std::move(files);
            }
            if (irDue_ && !hasQueued_ && !stop_)
            {
                // The IR of the program that plays, which its compile did not keep: compiled again for it alone, nothing
                // offered. Kept only if that program still plays and someone still looks.
                irDue_ = false;
                const Patch::Recipe recipe = recipe_;
                lock.unlock();
                Patch::Ir ir;
                std::string why;
                const bool made = recipe.program != 0 && Patch::ir(recipe, ir, why);
                std::shared_ptr<const IrText> lowered = made ? irText(ir.lowered) : nullptr;
                std::shared_ptr<const IrText> optimised = made ? irText(ir.optimised) : nullptr;
                lock.lock();
                if (irWanted_ && recipe.program != 0 && recipe_.program == recipe.program && !irDue_)
                {
                    ir_.program = recipe.program;
                    ir_.file = recipe.files.empty() ? std::string(kProgramName) : recipe.files.front().name;
                    ir_.lowered = std::move(lowered);
                    ir_.optimised = std::move(optimised);
                    ir_.why = made ? std::string() : why;
                }
                ir_.making = irDue_;
                bumpIr();
            }
            // The self-test's answer for the program that plays, compiled in when it may be (above), at a pass of its
            // own: never the one whose compile made it due (a RUN's waiter and the processor have that compile's status
            // first).
            std::unique_ptr<Built> stale = checkLanded();
            checkOutcomes();
            if (selfTesting_ && !compiled && mayRefine())
                refine(lock);
            lock.unlock();
            stale.reset();
            engine_.collect();                     // what the audio thread handed back
            lock.lock();
        }
        idle_.notify_all();
    }

    // ---- the self-test --------------------------------------------------------------------------------------------------
    void Compiler::played(Patch::Recipe recipe, std::vector<circuits::Job> jobs, uint64_t landed)
    {
        std::unique_ptr<Built> stale;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            recipe_ = std::move(recipe);
            playing_ = jobs;
            landedSeen_ = landed;                  // an answer that lands from now on is looked at (checkLanded)
            refineDue_ = false;
            stale = std::move(held_);              // a refine of the program before: never offered now
        }
        tester_.want(std::move(jobs));            // and a test the newest program does not want is cancelled
    }

    std::unique_ptr<Compiler::Built> Compiler::checkLanded()
    {
        const uint64_t landed = circuits::answers().landed();
        if (landed == landedSeen_)
            return nullptr;
        landedSeen_ = landed;
        bool due = false;
        for (const circuits::Job& j : playing_)
            if (circuits::answers().find(j.key))
                due = true;
        if (!due)
            return nullptr;
        refineDue_ = true;
        return std::move(held_);                   // built before this answer: compiled again, with it
    }

    void Compiler::checkOutcomes()
    {
        const uint64_t reported = circuits::answers().reported();
        if (reported == reportedSeen_)
            return;
        reportedSeen_ = reported;
        std::string notes = circuits::withOutcomes(status_.notes, playing_);
        if (notes == status_.notes)
            return;
        status_.notes = std::move(notes);
        status_.revision += 1;
        revision_.store(status_.revision, std::memory_order_release);
    }

    bool Compiler::quiet() const
    {
        if (engine_.ownCode())
            return false;                          // a recording is armed: what plays stays what it records
        if constexpr (product::kIsInstrument)
        {
            Meters m;
            if (!engine_.meters(m) || m.voices != 0 || m.fading != 0 || m.held != 0)
                return false;                      // only in silence: no held note starts again
        }
        return true;
    }

    bool Compiler::mayRefine() const
    {
        if (!(refineDue_ || held_) || hasQueued_ || compiling_ || stop_ || recipe_.program == 0)
            return false;
        return quiet();
    }

    bool Compiler::refine(std::unique_lock<std::mutex>& lock)
    {
        if (!held_)
        {
            // The program that plays (the recipe's files: never the draft, never a failed RUN's), compiled again under a
            // new program id, now with the answers stored.
            refineDue_ = false;
            Sources files = recipe_.files;
            const uint64_t base = recipe_.program;
            const uint64_t id = ++nextId_;
            const ShaderTools tools = tools_;
            const bool keepIr = irWanted_;
            const uint64_t landed = circuits::answers().landed();
            compiling_ = true;
            lock.unlock();
            std::unique_ptr<Built> b = build(engine_.rate(), std::move(files), id, tools, shaders_, keepIr);
            b->landed = landed;
            lock.lock();
            compiling_ = false;
            idle_.notify_all();
            wake_.notify_all();                    // a RUN that waited for it (selfTestNow's, on another thread)
            if (!b->patch)
            {
                // It compiled before, so this should not happen: say so, and let the provisional configuration play on
                // (until another answer lands, or another compile).
                CompileStatus s = status_;
                const std::string& e = b->status.errors;
                for (const circuits::Job& j : playing_)
                    if (circuits::answers().find(j.key))
                        s.notes += j.file + ": note: the self-test's configuration does not compile in this program, so the "
                                   "provisional one plays on: " + e.substr(0, e.find('\n')) + "\n";
                s.revision = status_.revision + 1;
                status_ = std::move(s);
                revision_.store(status_.revision, std::memory_order_release);
                lock.unlock();
                b.reset();
                lock.lock();
                return false;
            }
            if (recipe_.program != base || refineDue_ || stop_)
            {
                // Another program plays, or another answer landed meanwhile (compiled again with it, later).
                lock.unlock();
                b.reset();
                lock.lock();
                return false;
            }
            held_ = std::move(b);
        }
        // Looked at again now that it is built: a RUN queued since (which plays instead), a recording armed or a key
        // pressed while it compiled hold it, for the next pass that may.
        if (hasQueued_ || stop_ || !quiet())
            return false;
        std::unique_ptr<Built> b = std::move(held_);
        std::string chosen;
        for (const circuits::Job& j : playing_)
            if (const std::optional<std::string> answer = circuits::answers().find(j.key))
                chosen += circuits::chosenNote(j.file, *answer, j.key);
        engine_.offer(std::move(b->patch));        // under mutex_: no RUN is queued between the look and the offer
        compiling_ = true;                         // nothing else offers before its status is out
        lock.unlock();
        played(std::move(b->recipe), std::move(b->jobs), b->landed);
        tookProgram(b->status.request, fileOf(b->status), b->keepIr, b->ir);
        finishRefine(std::move(b->status), chosen);
        b.reset();
        lock.lock();
        compiling_ = false;
        idle_.notify_all();
        wake_.notify_all();
        return true;
    }

    void Compiler::finishRefine(CompileStatus s, const std::string& chosen)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        CompileStatus out;
        if (status_.ok)
        {
            out = std::move(s);                    // what plays: the refine's (the same files and dials)
            out.request = status_.request;         // still the answer to the request before it
        }
        else
        {
            out = status_;                         // a failed compile's outcome stays: its errors, its files
            out.program = s.program;
            out.circuits = std::move(s.circuits);
        }
        out.notes += chosen;
        out.refined = true;
        out.revision = status_.revision + 1;
        status_ = std::move(out);
        revision_.store(status_.revision, std::memory_order_release);
    }

    void Compiler::setSelfTesting(bool on)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            selfTesting_ = on;
        }
        tester_.setEnabled(on);
        wake_.notify_all();
    }

    bool Compiler::selfTestNow()
    {
        tester_.runNow();
        std::unique_ptr<Built> stale;
        bool offered = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stale = checkLanded();
            checkOutcomes();
            if (mayRefine())
                offered = refine(lock);
        }
        return offered;
    }

    bool Compiler::selfTesting() const
    {
        if (tester_.busy())
            return true;
        const std::lock_guard<std::mutex> lock(mutex_);
        return refineDue_ || held_ || std::any_of(playing_.begin(), playing_.end(), [](const circuits::Job& j) {
                   return circuits::answers().find(j.key).has_value();
               });
    }
}
