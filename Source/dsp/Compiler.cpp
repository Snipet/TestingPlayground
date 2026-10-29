// Source/dsp/Compiler.cpp: see Compiler.h.
#include "dsp/Compiler.h"

#include "dsp/Engine.h"
#include "fminus/Patch.h"

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

        // `keepIr`: the program's IR into `ir` too (Patch::compile's).
        CompileStatus build(Engine& engine, Sources files, uint64_t id, const ShaderTools& tools, ShaderCache& cache,
                            Patch::Recipe& recipe, bool keepIr, Patch::Ir& ir)
        {
            Patch::Result r = Patch::compile(files, engine.rate(), keepIr);
            ir = std::move(r.ir);
            CompileStatus s;
            s.request = id;
            s.ok = r.patch != nullptr;
            s.errors = std::move(r.errors);
            s.notes = std::move(r.notes);
            s.ms = r.compileMs;
            s.sources = std::move(files);
            if (r.patch)
            {
                s.controls = r.patch->controls();
                s.panel = r.patch->panel();
                if (!s.panel.shaders.empty())
                    buildShaders(s.panel, s.sources, tools, cache, s.notes);
                r.patch->setProgram(id);                    // CompileStatus::request: Meters::program names it
                recipe = r.patch->recipe();                 // the freezer's
                engine.offer(std::move(r.patch));
            }
            return s;
        }
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
            t->text = text;
        else
        {
            const std::size_t end = text.rfind('\n', kIrMaxBytes - 1);
            t->text = text.substr(0, end == std::string::npos ? kIrMaxBytes : end + 1);
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
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            id = ++nextId_;
        }
        ShaderTools tools;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            tools = tools_;
        }
        // Never the IR here (the calling thread may be the message thread): someone looking has it made on the
        // compiler's thread (tookProgram).
        Patch::Recipe recipe;
        Patch::Ir ir;
        CompileStatus s = build(engine_, std::move(files), id, tools, shaders_, recipe, false, ir);
        if (s.ok)
        {
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                recipe_ = std::move(recipe);
            }
            tookProgram(id, s.sources.empty() ? std::string(kProgramName) : s.sources.front().name, false, ir);
        }
        finish(s);
        return status();
    }

    void Compiler::finish(CompileStatus s)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        s.revision = status_.revision + 1;
        status_ = std::move(s);
        revision_.store(status_.revision, std::memory_order_release);
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
            wake_.wait_for(lock, std::chrono::milliseconds(100),
                           [this] { return hasQueued_ || hasCheck_ || irDue_ || stop_; });
            if (stop_)
                break;
            if (hasQueued_)
            {
                Sources files = std::move(queued_);
                const uint64_t id = queuedId_;
                const ShaderTools tools = tools_;
                const bool keepIr = irWanted_;         // someone looks: this compile keeps the program's IR
                hasQueued_ = false;
                compiling_ = true;
                lock.unlock();
                Patch::Recipe recipe;
                Patch::Ir ir;
                CompileStatus s = build(engine_, std::move(files), id, tools, shaders_, recipe, keepIr, ir);
                if (s.ok)
                {
                    lock.lock();
                    recipe_ = std::move(recipe);
                    lock.unlock();
                    // Its IR before its status: a view that sees the status has the text.
                    tookProgram(id, s.sources.empty() ? std::string(kProgramName) : s.sources.front().name, keepIr, ir);
                }
                finish(std::move(s));
                lock.lock();
                compiling_ = false;
                if (!hasQueued_)
                    idle_.notify_all();
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
            lock.unlock();
            engine_.collect();                     // what the audio thread handed back
            lock.lock();
        }
        idle_.notify_all();
    }
}
