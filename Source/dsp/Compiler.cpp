// Source/dsp/Compiler.cpp: see Compiler.h.
#include "dsp/Compiler.h"

#include "dsp/Engine.h"
#include "fminus/CirDoc.h"
#include "fminus/Circuits.h"
#include "fminus/GuiSpec.h"
#include "fminus/Patch.h"

#include "Product.h"

#include <algorithm>
#include <chrono>
#include <system_error>
#include <thread>
#include <utility>

namespace playground
{
    namespace
    {
        // The renderer's reason, as one line of a note (Metal's log after FunkGui's sentence, joined and cut).
        std::string noteLine(const std::string& why)
        {
            std::string out;
            for (const char c : why)
                out += c == '\n' || c == '\r' || c == '\t' ? ' ' : c;
            while (out.find("  ") != std::string::npos)
                out.erase(out.find("  "), 1);
            if (out.size() > 400)
                out = out.substr(0, 397) + "...";
            return out;
        }

        // The jobs for `shaders`, each from its file among `files` (one with no file is left out: GuiSpec said so), with
        // the program's .sh files it may include; `which`, when given, gets each job's shader.
        std::vector<ShaderJob> shaderJobs(const std::vector<PanelShader>& shaders, const Sources& files,
                                          std::vector<std::size_t>* which = nullptr)
        {
            std::vector<ShaderJob> jobs;
            const Sources headers = shaderHeaders(files);
            for (std::size_t k = 0; k < shaders.size(); ++k)
            {
                const PanelShader& sh = shaders[k];
                const auto f = std::find_if(files.begin(), files.end(), [&](const SourceFile& s) { return s.name == sh.file; });
                if (f == files.end())
                    continue;
                jobs.push_back({ sh.file, f->text, sh.inputs, headers });
                if (which != nullptr)
                    which->push_back(k);
            }
            return jobs;
        }

        // The panel's shaders built (their binaries in it), each from its file, and checked by the renderer, at once
        // (ShaderCache::buildAll) on threads of their own while the thread that compiles waits (the compiler's, or
        // compileNow's caller's: ShaderTools::check, so the Panel's shaders::set() then compiles nothing on the message
        // thread); a failure appended to `notes`, and the shader left without a binary.
        void buildShaders(PanelSpec& panel, const Sources& files, const ShaderTools& tools, ShaderCache& cache,
                          std::string& notes)
        {
            std::vector<std::size_t> which;
            const std::vector<ShaderJob> jobs = shaderJobs(panel.shaders, files, &which);
            const std::vector<ShaderBuild> builds = cache.buildAll(jobs, tools);
            for (std::size_t j = 0; j < builds.size(); ++j)
            {
                PanelShader& sh = panel.shaders[which[j]];
                const ShaderBuild& b = builds[j];
                if (b.ok() && b.refused)
                {
                    const std::string line = sh.file + ":1:1: note: the GPU refused it: "
                                           + (b.refusal.empty() ? std::string("no reason given") : noteLine(b.refusal)) + "\n";
                    if (notes.find(line) == std::string::npos)
                        notes += line;
                }
                else if (b.ok())
                {
                    sh.binary = b.binary;
                    sh.id = shaderId(sh.binary);
                }
                else if (notes.find(b.error) == std::string::npos)
                    notes += b.error;
            }
        }

        // The shaders a program's .gui file asks for, built (and checked) on a thread of its own while FMinus compiles
        // the program (readGuiShaders: their inputs taken at their word, before the program says which it has), so
        // that buildShaders, after the compile, finds them made: a RUN waits for the longer of the two, not their sum.
        // Joined before buildShaders runs (`wait`) and when it goes.
        class EarlyShaders
        {
        public:
            EarlyShaders(const Sources& files, const ShaderTools& tools, ShaderCache& cache)
            {
                if (!tools.usable())
                    return;
                const auto gui = std::find_if(files.begin(), files.end(), [](const SourceFile& f) { return isGuiFile(f.name); });
                if (gui == files.end())
                    return;
                std::vector<ShaderJob> jobs = shaderJobs(readGuiShaders(*gui, product::kIsInstrument), files);
                if (jobs.empty())
                    return;
                try
                {
                    thread_ = std::thread([&cache, tools, guessed = std::move(jobs)] { cache.buildAll(guessed, tools); });
                }
                catch (const std::system_error&)
                {
                }                                    // no thread: buildShaders builds them after the compile
            }
            ~EarlyShaders() { wait(); }
            EarlyShaders(const EarlyShaders&) = delete;
            EarlyShaders& operator=(const EarlyShaders&) = delete;

            void wait()
            {
                if (thread_.joinable())
                    thread_.join();
            }

        private:
            std::thread thread_;
        };

        // Whether `after` is a panel's edit of `before` (Compiler.h, "A panel's edit"): the same files but the panel's
        // (isPanelFile), in the same order, at least one of those changed, added or gone and none that FMinus read
        // (`read`), and both with the same first .gui file.
        bool panelEditOf(const Sources& before, const Sources& after, const std::vector<std::string>& read)
        {
            const auto program = [](const Sources& files) {
                std::vector<const SourceFile*> out;
                for (const SourceFile& f : files)
                    if (!isPanelFile(f.name))
                        out.push_back(&f);
                return out;
            };
            const std::vector<const SourceFile*> a = program(before), b = program(after);
            if (a.size() != b.size() || a.empty() || before.front().name != after.front().name)
                return false;
            for (std::size_t k = 0; k < a.size(); ++k)
                if (!(*a[k] == *b[k]))
                    return false;
            const auto gui = [](const Sources& files) -> const SourceFile* {
                for (const SourceFile& f : files)
                    if (isGuiFile(f.name))
                        return &f;
                return nullptr;
            };
            const SourceFile* ga = gui(before);
            const SourceFile* gb = gui(after);
            if (ga == nullptr || gb == nullptr || ga->name != gb->name)
                return false;
            bool changed = false;
            const auto touched = [&](const std::string& name) {
                changed = true;
                return std::find(read.begin(), read.end(), name) != read.end();
            };
            for (const SourceFile& f : before)
                if (isPanelFile(f.name))
                {
                    const SourceFile* now = findFile(after, f.name);
                    if ((now == nullptr || now->text != f.text) && touched(f.name))
                        return false;
                }
            for (const SourceFile& f : after)
                if (isPanelFile(f.name) && findFile(before, f.name) == nullptr && touched(f.name))
                    return false;
            return changed;
        }

        // Whether two declarations of a dial mean the same values (a panel's edit keeps them): all but how a value
        // reads (its unit, decimals and scale).
        bool sameMeaning(const Control& a, const Control& b)
        {
            return a.used == b.used && sameName(a, b) && a.type == b.type && a.min == b.min && a.max == b.max
                && a.def == b.def && a.taper == b.taper;
        }

        // A status's notes with the panel's read again: the lines of the panel's files of before and of after left
        // out ("panel.gui:12:5: note: ..."), `panel` (its notes now) after the rest.
        std::string withPanelNotes(const std::string& notes, const Sources& before, const Sources& after,
                                   const std::string& panel)
        {
            std::string out;
            for (std::size_t at = 0; at < notes.size();)
            {
                const std::size_t end = std::min(notes.find('\n', at), notes.size());
                const std::string line = notes.substr(at, end - at);
                const std::size_t colon = line.find(':');
                const std::string file = colon == std::string::npos ? std::string() : line.substr(0, colon);
                const bool panelLine = isPanelFile(file) && (findFile(before, file) != nullptr || findFile(after, file) != nullptr);
                if (!panelLine && !line.empty())
                    out += line + "\n";
                at = end + 1;
            }
            return out + panel;
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
                                                     ShaderCache& cache, bool keepIr, const CircuitPins* pins,
                                                     const Assets& assets)
    {
        auto b = std::make_unique<Built>();
        EarlyShaders early(files, tools, cache);
        Patch::Result r = Patch::compile(files, rate, keepIr, pins, true, true, assets);
        b->ir = std::move(r.ir);
        b->keepIr = keepIr;
        b->jobs = std::move(r.circuitJobs);
        CompileStatus& s = b->status;
        s.request = id;
        s.ok = r.patch != nullptr;
        s.stopped = r.stopped != Patch::Method::none;     // an init that ran past its budget (Patch.h)
        s.bounds = s.stopped && r.bounds.failed;          // or failed a bounds check
        s.stoppedProgram = s.stopped ? id : 0;
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
            {
                early.wait();
                buildShaders(s.panel, s.sources, tools, cache, s.notes);
            }
            r.patch->setProgram(id);
            s.program = id;
            b->recipe = r.patch->recipe();
            s.circuits = b->recipe.pins;
            s.circuitDials = b->recipe.bindings;
            s.probes = b->recipe.probes;
            s.samples = b->recipe.samples;
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
            const uint64_t stopped = stoppedProgram_;
            lock.unlock();
            DialState d;
            const bool read = engine_.dials(d);
            if (engine_.ownCode())
                triedProgram_ = 0;                     // a recording plays the program's own code: after it, try again
            else if (on && read && d.program != 0 && d.program != stopped)   // (a stopped program plays no more)
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

    uint64_t Compiler::request(Sources files, bool panelEdits)
    {
        uint64_t id = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            queued_ = std::move(files);
            queuedPanel_ = panelEdits;
            hasQueued_ = true;
            id = queuedId_ = ++nextId_;
        }
        wake_.notify_all();
        return id;
    }

    bool Compiler::withdraw(uint64_t id)
    {
        Sources files;                             // let go of outside the lock
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (id == 0 || !hasQueued_ || queuedId_ != id)
                return false;
            hasQueued_ = false;
            files = std::move(queued_);
            queued_.clear();
        }
        idle_.notify_all();                        // wait(), which the request held
        return true;
    }

    CompileStatus Compiler::compileNow(Sources files, bool carry)
    {
        uint64_t id = 0;
        ShaderTools tools;
        Assets assets;
        {
            // One compile offers at a time (above): the thread's (a RUN's, a refine's) ends first, and the thread starts
            // none while this one compiles, so this program, offered last, is the one that plays.
            std::unique_lock<std::mutex> lock(mutex_);
            idle_.wait(lock, [this] { return !compiling_ || stop_; });
            compiling_ = true;
            id = ++nextId_;
            tools = tools_;
            assets = assets_;
        }
        // Never the IR here (the calling thread may be the message thread): someone looking has it made on the
        // compiler's thread (tookProgram).
        const uint64_t landed = circuits::answers().landed();
        CompileStatus s = land(build(engine_.rate(), std::move(files), id, tools, shaders_, false, nullptr, assets), landed,
                               carry);
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            compiling_ = false;
        }
        idle_.notify_all();
        wake_.notify_all();                        // a RUN that waited for it
        return s;
    }

    CompileStatus Compiler::land(std::unique_ptr<Built> b, uint64_t landed, bool carry, bool watch)
    {
        if (!b->patch)
            return finish(std::move(b->status));
        if (carry)
        {
            DialCarrier carrier;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                carrier = carrier_;
            }
            carryInto(*b, carrier);                // just before the offer: the dials as they are now
        }
        const uint64_t id = b->status.request;
        engine_.offer(std::move(b->patch), watch);
        if (watch && !firstBlocks(id))
        {
            // It stopped in its first blocks, and the Engine went back to the program it replaced, which plays on: the
            // RUN failed, as a compile that does not compile fails (above, "A program that stops").
            Engine::Stopped st;
            bool read = false;
            for (int k = 0; k < 1000 && !(read = engine_.lastStop(st) && st.program == id); ++k)
                std::this_thread::yield();        // published just after the Engine claimed the revert
            CompileStatus s = std::move(b->status);
            s.ok = false;
            s.stopped = true;
            s.stoppedProgram = id;
            s.bounds = read && st.bounds.failed;
            s.errors = Patch::stopMessage(s.sources, read ? st.call : Patch::Method::process, engine_.rate(),
                                          read ? &st.bounds : nullptr);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                stoppedProgram_ = id;
            }
            return finish(std::move(s));
        }
        played(std::move(b->recipe), std::move(b->jobs), landed, carry);
        // Its IR before its status: a view that sees the status has the text.
        tookProgram(b->status.request, fileOf(b->status), b->keepIr, b->ir);
        CompileStatus s = finish(std::move(b->status));
        if (!watch)
            return s;
        std::unique_lock<std::mutex> lock(mutex_);
        checkStops(lock);                          // one that stopped with nothing to go back to: said at once
        return status_;
    }

    bool Compiler::firstBlocks(uint64_t program)
    {
        // Until it plays a block with no swap under way, the Engine goes back from it, or the audio thread stops: no
        // block under way, and none ended for three of its own (20 ms at least: none ever came, or a host that calls
        // none). A block under way may hold a call that runs past its budget, which takes as long as the budget's
        // iterations do at its loop's pace: waited for as long as the largest call's budget (a block of prepare()'s
        // most samples) takes at kSlowLoopsPerSecond (2^24: a loop whose iteration takes some 256 cycles), ten seconds
        // at least. A call slower still is said when it stops, as a later stop is (checkStops).
        using namespace std::chrono;
        constexpr double kSlowLoopsPerSecond = 16'777'216.0;
        const double most = static_cast<double>(Patch::budgetOf(std::max(engine_.largestBlock(), 1), engine_.rate()))
                          / kSlowLoopsPerSecond;
        const auto cap = duration<double>(std::max(10.0, most));
        const auto start = steady_clock::now();
        while (engine_.settledProgram() != program && engine_.revertible() == program && !audioIdle()
               && steady_clock::now() - start < cap)
            std::this_thread::sleep_for(milliseconds(1));
        return engine_.settle(program);
    }

    bool Compiler::audioIdle() const
    {
        using namespace std::chrono;
        const int64_t began = engine_.lastBlockStartNs(), ended = engine_.lastBlockNs();
        if (began > ended)
            return false;
        const int64_t now = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
        const double gap = std::max(0.02, 3.0 * engine_.lastBlockSeconds());
        return static_cast<double>(now - ended) * 1e-9 > gap;
    }

    // ---- a panel's edit (Compiler.h) ---------------------------------------------------------------------------------
    bool Compiler::editPanel(const Sources& files, uint64_t id, const ShaderTools& tools, const Assets& assets)
    {
        const auto t0 = std::chrono::steady_clock::now();
        Patch::Recipe base;
        CompileStatus was;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (recipe_.program == 0 || recipe_.program == stoppedProgram_ || held_ || !status_.ok
                || status_.program != recipe_.program || (recipe_.sharedTick && (refineDue_ || answered())))
                return false;                          // nothing plays, it stopped, a refine waits, the status is not
                                                       // of what plays, or an answer waits for the next RUN (a Shared
                                                       // that ticks is never refined): this RUN compiles to play it
            base = recipe_;
            was = status_;
        }
        if (!panelEditOf(base.files, files, base.panel.read))
            return false;
        for (const auto& [name, hash] : base.samples)   // a sample it reads added, changed or gone: a compile
        {
            const std::shared_ptr<const assets::Asset> now = assets != nullptr ? assets->find(name) : nullptr;
            if ((now != nullptr ? now->hash() : std::string()) != hash)
                return false;
        }
        // The panel as it plays (its taps: the program's own, or a panel's edit's before this one), and as it is now.
        Controls before, after;
        std::vector<Patch::Tap> tapsBefore, tapsAfter;
        std::string ignored, notes;
        (void)Patch::readPanel(base.panel, base.controls, base.files, before, tapsBefore, ignored);
        PanelSpec panel = Patch::readPanel(base.panel, base.controls, files, after, tapsAfter, notes);
        for (int c = 0; c < kMaxControls; ++c)
        {
            const auto i = static_cast<std::size_t>(c);
            if (!sameMeaning(was.controls[i], after[i]) || (after[i].used && was.panel.options[i] != panel.options[i]))
                return false;                          // a dial's declaration changed: the carry's, a compile's
        }
        if (!panel.shaders.empty())
            buildShaders(panel, files, tools, shaders_, notes);
        CompileStatus s = std::move(was);
        uint64_t taps = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (recipe_.program != base.program || held_)
                return false;                          // (nothing offers meanwhile: this thread compiles)
            if (tapsAfter != tapsBefore)
            {
                Engine::TapTable t;
                t.program = base.program;
                t.count = static_cast<uint32_t>(std::min<std::size_t>(tapsAfter.size(), kMaxDisplays));
                std::copy_n(tapsAfter.begin(), t.count, t.taps.begin());
                taps = engine_.setTaps(t);
            }
            recipe_.files = files;                     // what plays now: a refine, a restore and the freezer read them
        }
        s.request = id;
        s.notes = withPanelNotes(s.notes, base.files, files, notes);
        s.sources = files;
        s.controls = after;
        panel.tapTable = taps != 0 ? taps : s.panel.tapTable;   // the table its displays' taps index (Engine::setTaps)
        s.panel = std::move(panel);
        // The carry stays the program's: when poll() never saw its compile's status (this one came first), this one
        // writes it, and when it did, writes nothing again (a carry writes once).
        s.refined = false;
        s.panelOnly = true;
        // The displays' new fields read by the audio thread before the status is out, unless it is idle (a block, or
        // a few of its own at most).
        while (taps != 0 && engine_.tapsRead() < taps && !audioIdle()
               && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        s.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        finish(std::move(s));
        return true;
    }

    void Compiler::checkStops(std::unique_lock<std::mutex>& lock)
    {
        const uint64_t count = engine_.stops();
        if (count == stopsSeen_)
            return;
        stopsSeen_ = count;
        Engine::Stopped st;
        bool read = false;
        for (int k = 0; k < 100 && !(read = engine_.lastStop(st)); ++k)
            std::this_thread::yield();
        if (!read || st.reverted || st.program == 0 || st.program != recipe_.program || st.program == stoppedProgram_)
            return;                                // land() said so, or it is not the program that plays
        stoppedProgram_ = st.program;
        const float rate = engine_.rate();
        CompileStatus s = status_;
        s.ok = false;
        s.stopped = true;
        s.stoppedProgram = st.program;
        s.refined = false;
        s.bounds = st.bounds.failed;
        s.errors = Patch::stopMessage(recipe_.files, st.call, rate, &st.bounds);
        s.sources = recipe_.files;
        s.restored.clear();
        s.program = st.program;                    // (silent) until the program before it plays again
        if (previous_.program != 0 && !previous_.files.empty())
        {
            // The program before it, compiled again with its netlists' configurations and offered as a refine is.
            const Patch::Recipe before = previous_;
            const uint64_t id = ++nextId_;
            const ShaderTools tools = tools_;
            const bool keepIr = irWanted_;
            const uint64_t landed = circuits::answers().landed();
            const bool was = compiling_;
            compiling_ = true;
            lock.unlock();
            std::unique_ptr<Built> b = build(rate, before.files, id, tools, shaders_, keepIr, &before.pins, before.assets);
            lock.lock();
            if (b->patch && recipe_.program == st.program)   // (else another program plays now)
            {
                carryInto(*b, carrier_);           // the dials as they are now, under mutex_ (as a refine's)
                engine_.offer(std::move(b->patch));
                s.program = id;
                s.restored = b->status.sources;
                s.controls = b->status.controls;
                s.panel = b->status.panel;
                s.carry = b->status.carry;
                s.notes = b->status.notes;
                s.circuitKeys = b->status.circuitKeys;
                s.circuits = b->status.circuits;
                s.circuitDials = b->status.circuitDials;
                s.probes = b->status.probes;
                s.samples = b->status.samples;
                lock.unlock();
                played(std::move(b->recipe), std::move(b->jobs), landed, false);
                tookProgram(id, fileOf(b->status), b->keepIr, b->ir);
                b.reset();
                lock.lock();
            }
            else
            {
                lock.unlock();
                b.reset();
                lock.lock();
            }
            compiling_ = was;
            idle_.notify_all();
            wake_.notify_all();
            if (recipe_.program != st.program && recipe_.program != s.program)
                return;                            // a RUN landed meanwhile: its status stands
        }
        s.revision = status_.revision + 1;
        status_ = std::move(s);
        revision_.store(status_.revision, std::memory_order_release);
    }

    CompileStatus Compiler::finish(CompileStatus s)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!s.ok)
        {
            s.program = recipe_.program;           // the program that played plays on
            s.circuits = recipe_.pins;
            s.circuitDials = recipe_.bindings;
            s.probes = recipe_.probes;
            s.samples = recipe_.samples;
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

    void Compiler::setAssets(Assets assets)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        assets_ = std::move(assets);
    }

    void Compiler::setDialCarrier(DialCarrier carrier)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        carrier_ = std::move(carrier);
    }

    void Compiler::carryInto(Built& b, const DialCarrier& carrier)
    {
        if (!b.patch || !carrier)
            return;
        b.status.carry = carrier(b.status.controls, b.status.panel);
        b.patch->setCarry(b.status.carry);
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
                const bool panelEdits = queuedPanel_;
                const Assets assets = assets_;
                hasQueued_ = false;
                compiling_ = true;
                lock.unlock();
                const uint64_t landed = circuits::answers().landed();
                // An edit of the panel's files alone plays on (Compiler.h, "A panel's edit"); else a compile.
                if (!(panelEdits && editPanel(files, id, tools, assets)))
                    land(build(engine_.rate(), std::move(files), id, tools, shaders_, keepIr, nullptr, assets), landed, true,
                         true);
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
            // A program that stopped as it played (above, "A program that stops"): said, and the one before it played
            // again.
            if (!hasQueued_ && !stop_)
                checkStops(lock);
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
    void Compiler::played(Patch::Recipe recipe, std::vector<circuits::Job> jobs, uint64_t landed, bool previous)
    {
        std::unique_ptr<Built> stale;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            // The program before, to play again should this one stop (above, "A program that stops"): one of other files
            // (a refine's are the same), never one that stopped.
            Engine::Stopped st;                    // (one that stopped since this thread looked: the newest stop)
            const bool stopped = recipe_.program == stoppedProgram_
                              || (engine_.lastStop(st) && st.program == recipe_.program && !st.reverted);
            if (!previous)
                previous_ = {};
            else if (recipe_.program != 0 && !stopped && !cir::sameProgram(recipe_.files, recipe.files))
                previous_ = recipe_;
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
            if (recipe_.sharedTick)
                return false;                      // its Shared moves between notes (Patch.h, "Shared on its own"), and a
                                                   // new program's starts afresh: the answer waits for the next RUN
            Meters m;
            if (!engine_.meters(m) || m.voices != 0 || m.fading != 0 || m.held != 0)
                return false;                      // only in silence: no held note starts again
        }
        return true;
    }

    bool Compiler::mayRefine() const
    {
        if (!(refineDue_ || held_) || hasQueued_ || compiling_ || stop_ || recipe_.program == 0
            || recipe_.program == stoppedProgram_)   // a program that stopped plays no more
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
            const Assets assets = recipe_.assets;     // the samples it plays with
            const uint64_t base = recipe_.program;
            const uint64_t id = ++nextId_;
            const ShaderTools tools = tools_;
            const bool keepIr = irWanted_;
            const uint64_t landed = circuits::answers().landed();
            compiling_ = true;
            lock.unlock();
            std::unique_ptr<Built> b = build(engine_.rate(), std::move(files), id, tools, shaders_, keepIr, nullptr, assets);
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
        // The program it refines may have stopped meanwhile (above, "A program that stops"): said now, and the refine
        // let go (the program before it played again let it go already; else nothing plays, and it is never refined).
        checkStops(lock);
        if (held_ && recipe_.program == stoppedProgram_)
        {
            std::unique_ptr<Built> stopped = std::move(held_);
            lock.unlock();
            stopped.reset();
            lock.lock();
        }
        if (!held_ || hasQueued_ || stop_)
            return false;
        std::unique_ptr<Built> b = std::move(held_);
        std::string chosen;
        for (const circuits::Job& j : playing_)
            if (const std::optional<std::string> answer = circuits::answers().find(j.key))
                chosen += circuits::chosenNote(j.file, *answer, j.key);
        carryInto(*b, carrier_);                   // the dials as they are now (a refine held since it compiled)
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
            out.carry = s.carry;
            out.circuits = std::move(s.circuits);
            out.circuitDials = std::move(s.circuitDials);
            out.probes = std::move(s.probes);
            out.samples = std::move(s.samples);
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
        return refineDue_ || held_ || answered();
    }

    bool Compiler::answered() const
    {
        return std::any_of(playing_.begin(), playing_.end(),
                           [](const circuits::Job& j) { return circuits::answers().find(j.key).has_value(); });
    }
}
