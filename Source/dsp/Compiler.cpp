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

        CompileStatus build(Engine& engine, Sources files, uint64_t id, const ShaderTools& tools, ShaderCache& cache,
                            Patch::Recipe& recipe)
        {
            Patch::Result r = Patch::compile(files, engine.rate());
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
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (recipe_.program == 0 || recipe_.program != d.program)
                return false;
            recipe = recipe_;
        }
        std::string why;
        std::unique_ptr<Patch::Frozen> f = Patch::freeze(recipe, d.plain, why);
        if (!f)
            return false;
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
            if (on && engine_.dials(d) && d.program != 0)
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
            lock.lock();
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
        Patch::Recipe recipe;
        CompileStatus s = build(engine_, std::move(files), id, tools, shaders_, recipe);
        if (s.ok)
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            recipe_ = std::move(recipe);
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
            wake_.wait_for(lock, std::chrono::milliseconds(100), [this] { return hasQueued_ || stop_; });
            if (stop_)
                break;
            if (hasQueued_)
            {
                Sources files = std::move(queued_);
                const uint64_t id = queuedId_;
                const ShaderTools tools = tools_;
                hasQueued_ = false;
                compiling_ = true;
                lock.unlock();
                Patch::Recipe recipe;
                CompileStatus s = build(engine_, std::move(files), id, tools, shaders_, recipe);
                const bool ok = s.ok;
                finish(std::move(s));
                lock.lock();
                if (ok)
                    recipe_ = std::move(recipe);
                compiling_ = false;
                if (!hasQueued_)
                    idle_.notify_all();
            }
            lock.unlock();
            engine_.collect();                     // what the audio thread handed back
            lock.lock();
        }
        idle_.notify_all();
    }
}
