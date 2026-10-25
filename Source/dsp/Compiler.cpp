// Source/dsp/Compiler.cpp: see Compiler.h.
#include "dsp/Compiler.h"

#include "dsp/Engine.h"
#include "fminus/Patch.h"

#include <chrono>
#include <utility>

namespace testingplayground
{
    namespace
    {
        CompileStatus build(Engine& engine, Sources files, uint64_t id)
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
                r.patch->setProgram(id);                    // CompileStatus::request: Meters::program names it
                engine.offer(std::move(r.patch));
            }
            return s;
        }
    }

    Compiler::Compiler(Engine& engine) : engine_(engine), thread_([this] { run(); }) {}

    Compiler::~Compiler()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
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
        CompileStatus s = build(engine_, std::move(files), id);
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
                hasQueued_ = false;
                compiling_ = true;
                lock.unlock();
                CompileStatus s = build(engine_, std::move(files), id);
                finish(std::move(s));
                lock.lock();
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
