// Source/fminus/Patch.cpp: see Patch.h.
//
// compile() runs FMinus twice. The first pass, at -O0, reads the program as it is: does it compile, has it the main
// module and its methods, what are its parameters? Only then can the second choose the parameters' bindings (a float
// a stream), and append the plugin's calls (one function a method, one line each, after the program's last line), at
// -O2, flushing subnormals as the audio thread does (Program::Options::denormals). FMinus reports a mistake in how a
// method is declared as an error in one of those lines, which the program's author never wrote: such a message is
// rewritten to say which method the plugin calls, and how. Both passes read the program's other files through
// Program::Options::include, by name: an #include of a name no file has is FMinus's "'name' file not found".
#include "fminus/Patch.h"

#include "Product.h"

#include "fminus/FMinus.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>

namespace testingplaygroundsynth
{
    namespace
    {
        constexpr bool kInstrument = product::kIsInstrument;
        constexpr const char* kModule = kInstrument ? "Voice" : "Effect";

        // One of the plugin's calls into the program: the method, whether the plugin needs it, how the plugin calls
        // it (as the author would declare it), and the line appended to the program for it.
        struct Call
        {
            const char* method;
            bool required;
            const char* declaration;
            const char* shim;
        };

        constexpr Call kEffectCalls[] = {
            { "process", true, "void process(float* restrict left, float* restrict right, int n)",
              "void playground_process(Effect* self, float* restrict left, float* restrict right, int n) "
              "[[clang::nonblocking]] { self->process(left, right, n); }" },
            { "init", false, "void init(float rate)",
              "void playground_init(Effect* self, float rate) { self->init(rate); }" },
            { "end", false, "void end()", "void playground_end(Effect* self) { self->end(); }" },
        };

        constexpr Call kVoiceCalls[] = {
            { "start", true, "void start(float note, float velocity)",
              "void playground_start(Voice* self, float note, float velocity) [[clang::nonblocking]] "
              "{ self->start(note, velocity); }" },
            { "process", true, "bool process(float* restrict left, float* restrict right, int n)",
              "bool playground_process(Voice* self, float* restrict left, float* restrict right, int n) "
              "[[clang::nonblocking]] { return self->process(left, right, n); }" },
            { "stop", false, "void stop()",
              "void playground_stop(Voice* self) [[clang::nonblocking]] { self->stop(); }" },
            { "init", false, "void init(float rate)",
              "void playground_init(Voice* self, float rate) { self->init(rate); }" },
            { "end", false, "void end()", "void playground_end(Voice* self) { self->end(); }" },
        };

        template <std::size_t N>
        constexpr std::size_t countOf(const Call (&)[N]) noexcept { return N; }

        const Call* calls() noexcept { return kInstrument ? kVoiceCalls : kEffectCalls; }
        constexpr std::size_t callCount() noexcept
        {
            return kInstrument ? countOf(kVoiceCalls) : countOf(kEffectCalls);
        }

        // The audio thread's methods: the ones the plugin calls nonblocking.
        bool audioThread(const Call& c) noexcept
        {
            return std::strstr(c.shim, "[[clang::nonblocking]]") != nullptr;
        }

        int lineCount(std::string_view s) noexcept
        {
            return static_cast<int>(std::count(s.begin(), s.end(), '\n')) + 1;
        }

        double msSince(std::chrono::steady_clock::time_point t0)
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }

        // `errors` with each message about one of the appended lines (lines from `firstShim`, shims[k] on line
        // firstShim + k) said in the author's terms: the method, how the plugin calls it, and FMinus's reason.
        std::string translate(const std::string& errors, std::string_view fileName, int firstShim,
                              const std::vector<const Call*>& shims)
        {
            std::istringstream in(errors);
            std::string out, line;
            const std::string prefix = std::string(fileName) + ":";
            while (std::getline(in, line))
            {
                int ln = 0, col = 0, used = 0;
                if (line.rfind(prefix, 0) == 0
                    && std::sscanf(line.c_str() + prefix.size(), "%d:%d: %n", &ln, &col, &used) >= 2 && used > 0
                    && ln >= firstShim && ln - firstShim < static_cast<int>(shims.size()))
                {
                    const Call& c = *shims[static_cast<std::size_t>(ln - firstShim)];
                    std::string rest = line.substr(prefix.size() + static_cast<std::size_t>(used));   // "error: ..."
                    const std::size_t colon = rest.find(": ");
                    const std::string kind = colon == std::string::npos ? "error" : rest.substr(0, colon);
                    const std::string why = colon == std::string::npos ? rest : rest.substr(colon + 2);
                    out += prefix + " " + kind + ": " + kModule + "::" + c.method + ": ";
                    if (why.find("nonblocking") != std::string::npos)
                        out += "it runs on the audio thread, so it must not reach calloc, malloc or free (the note "
                               "below says where it does)";
                    else
                        out += std::string("the plugin calls it as `") + c.declaration + "`: " + why;
                    out += "\n";
                    continue;
                }
                out += line + "\n";
            }
            return out;
        }

        // The base module name and element count of a field type: "Osc" (1), "Osc[8]" (8), "Osc[2][3]" (6); a pointer
        // ("Osc *") is no module held by value: false.
        bool heldModule(const std::string& type, std::string& base, std::size_t& count)
        {
            if (type.find('*') != std::string::npos)
                return false;
            const std::size_t open = type.find('[');
            base = type.substr(0, open);
            while (!base.empty() && base.back() == ' ')
                base.pop_back();
            count = 1;
            for (std::size_t at = open; at != std::string::npos; at = type.find('[', at + 1))
                count *= static_cast<std::size_t>(std::max(1L, std::strtol(type.c_str() + at + 1, nullptr, 10)));
            return !base.empty();
        }

        Control controlOf(const fminus::Program::Parameter& p)
        {
            Control c;
            std::snprintf(c.name, sizeof c.name, "%s", p.name.c_str());
            c.used = true;
            if (p.type == "bool")
            {
                c.type = ControlType::toggle;
                c.min = 0.0f;
                c.max = 1.0f;
            }
            else
            {
                c.type = p.type == "int" ? ControlType::integer : ControlType::real;
                c.min = static_cast<float>(p.minimum.value_or(0.0));
                c.max = static_cast<float>(p.maximum.value_or(c.type == ControlType::integer ? 10.0 : 1.0));
                if (c.max < c.min)
                    std::swap(c.min, c.max);
                if (!(c.max > c.min))
                    c.max = c.min + 1.0f;
            }
            const double def = p.defaultValue.value_or(static_cast<double>(c.min));
            c.def = plainOf(c, normOf(c, static_cast<float>(def)));
            return c;
        }
    }

    struct Patch::Shared
    {
        using Process = void(void*, float*, float*, int);
        using Render = bool(void*, float*, float*, int);
        using Start = void(void*, float, float);
        using Init = void(void*, float);
        using Plain = void(void*);

        std::unique_ptr<fminus::Program> program;
        const fminus::Program::Module* module = nullptr;
        Process* process = nullptr;
        Render* render = nullptr;
        Start* start = nullptr;
        Plain* stop = nullptr;
        Init* init = nullptr;
        Plain* end = nullptr;
    };

    Patch::Patch() : s_(std::make_unique<Shared>()) {}

    Patch::~Patch()
    {
        freeInstances();
    }

    void Patch::freeInstances() noexcept
    {
        for (std::size_t i = 0; i < instances_.size(); ++i)
        {
            if (initialised_[i] && s_->end != nullptr)
                s_->end(instances_[i]);
            std::free(instances_[i]);
        }
        instances_.clear();
        initialised_.clear();
    }

    Patch::Result Patch::compile(std::string_view source, float sampleRate, std::string_view fileName)
    {
        return compile(singleFile(std::string(source), std::string(fileName)), sampleRate);
    }

    Patch::Result Patch::compile(const Sources& files, float sampleRate)
    {
        const auto t0 = std::chrono::steady_clock::now();
        Result r;
        if (files.empty())
        {
            r.errors = std::string(kProgramName) + ": error: there is no program\n";
            return r;
        }
        const std::string file = files.front().name;
        const std::string_view source = files.front().text;
        // An #include finds the file of that name; the program's own included again reads nothing, as FMinus reads
        // each file once. The files read, in the order first read: the panel's lines come from them.
        std::vector<const SourceFile*> compiled{ &files.front() };
        const fminus::Program::IncludeResolver include =
            [&files, &compiled](std::string_view name, std::string_view) -> std::optional<fminus::Program::IncludedFile> {
            if (const SourceFile* f = findFile(files, name))
            {
                if (std::find(compiled.begin(), compiled.end(), f) == compiled.end())
                    compiled.push_back(f);
                return fminus::Program::IncludedFile{ f->name, f->text };
            }
            return std::nullopt;
        };

        // Pass 1: the program as written.
        fminus::Program::Options read;
        read.optimisation = 0;
        read.knobVersions = 0;
        read.fileName = file;
        read.include = include;
        std::unique_ptr<fminus::Program> first = fminus::Program::compile(source, r.errors, read);
        if (!first)
        {
            r.compileMs = msSince(t0);
            return r;
        }
        const fminus::Program::Module* main = first->module(kModule);
        if (main == nullptr)
        {
            std::string have;
            for (const fminus::Program::Module& m : first->modules())
                have += (have.empty() ? "" : ", ") + m.name;
            r.errors = file + ": error: the plugin plays a module " + kModule + " (README.md, \"Writing a program\")" +
                       (have.empty() ? std::string(", and the program has no module") : ", and the program has " + have) +
                       "\n";
            r.compileMs = msSince(t0);
            return r;
        }
        const auto has = [&](const char* method) {
            const std::string name = std::string(kModule) + "::" + method;
            return std::find(main->methods.begin(), main->methods.end(), name) != main->methods.end();
        };
        std::vector<const Call*> shims;
        for (std::size_t k = 0; k < callCount(); ++k)
        {
            const Call& c = calls()[k];
            if (has(c.method))
                shims.push_back(&c);
            else if (c.required)
                r.errors += file + ": error: " + kModule + " needs a method `" + c.declaration + "`" +
                            (audioThread(c) ? ", which the plugin calls on the audio thread" : "") + "\n";
        }
        if (!r.errors.empty())
        {
            r.compileMs = msSince(t0);
            return r;
        }

        // The dials: the first kMaxControls parameters, a float a stream.
        auto patch = std::unique_ptr<Patch>(new Patch());
        fminus::Program::Options build;
        build.fileName = file;
        build.include = include;
        build.denormals = fminus::Program::Denormals::Flush;     // the audio thread's mode (ScopedNoDenormals)
        std::vector<std::string> names;
        for (std::size_t k = 0; k < main->parameters.size(); ++k)
        {
            const fminus::Program::Parameter& p = main->parameters[k];
            if (k >= static_cast<std::size_t>(kMaxControls))
            {
                r.notes += file + ": note: " + p.module + "::" + p.name + " has no dial (the plugin has " +
                           std::to_string(kMaxControls) + "): it stays at its default\n";
                continue;
            }
            patch->controls_[k] = controlOf(p);
            names.push_back(p.module + "::" + p.name);
            if (p.type == "float")
                build.parameters[names.back()] = fminus::Program::Binding::stream();
        }

        // Pass 2: the program and the plugin's calls, one line each after its last.
        std::string full(source);
        if (!full.empty() && full.back() != '\n')
            full += '\n';
        const int firstShim = lineCount(full);
        for (const Call* c : shims)
            full += std::string(c->shim) + "\n";
        std::string errors;
        patch->s_->program = fminus::Program::compile(full, errors, build);
        if (!patch->s_->program)
        {
            r.errors = translate(errors, file, firstShim, shims);
            r.compileMs = msSince(t0);
            return r;
        }
        fminus::Program& program = *patch->s_->program;
        Shared& s = *patch->s_;
        s.module = program.module(kModule);
        const auto look = [&](const char* method, auto*& fn) {
            if (!has(method))
                return;
            std::string missing;
            fn = program.function<std::remove_pointer_t<std::remove_reference_t<decltype(fn)>>>(
                std::string("playground_") + method, missing);
            r.errors += missing;
        };
        if constexpr (kInstrument)
        {
            look("start", s.start);
            look("process", s.render);
            look("stop", s.stop);
        }
        else
        {
            look("process", s.process);
        }
        look("init", s.init);
        look("end", s.end);
        if (!r.errors.empty() || s.module == nullptr)
        {
            r.compileMs = msSince(t0);
            return r;
        }
        patch->params_.assign(static_cast<std::size_t>(kMaxControls), nullptr);
        for (std::size_t k = 0; k < names.size(); ++k)
            patch->params_[k] = program.parameter(names[k]);

        // The instances: aligned, zeroed, their parameters at their defaults, init run.
        patch->instanceAlign_ = std::max<std::size_t>(s.module->alignment, alignof(std::max_align_t));
        patch->instanceSize_ = std::max<std::size_t>(s.module->size, 1);
        patch->instanceSize_ = (patch->instanceSize_ + patch->instanceAlign_ - 1) / patch->instanceAlign_ *
                               patch->instanceAlign_;
        const int count = kInstrument ? kInstances : 1;
        for (int i = 0; i < count; ++i)
        {
            void* p = std::aligned_alloc(patch->instanceAlign_, patch->instanceSize_);
            if (p == nullptr)
            {
                r.errors = file + ": error: no memory for " + std::to_string(count) + " instances of " + kModule + "\n";
                r.compileMs = msSince(t0);
                return r;
            }
            patch->instances_.push_back(p);
            patch->initialised_.push_back(false);
        }
        patch->sources_ = files;
        patch->panel_ = readPanelSpec(compiled, patch->controls_, r.notes);
        patch->reset(sampleRate);
        r.compileMs = msSince(t0);
        r.patch = std::move(patch);
        return r;
    }

    void Patch::setDefaults(void* instance, std::string_view moduleName, int depth) const
    {
        const fminus::Program::Module* m = s_->program->module(moduleName);
        if (m == nullptr || depth > 16)
            return;
        fminus::Program::setDefaults(instance, *m);
        for (const fminus::Program::Module::Field& f : m->fields)
        {
            std::string base;
            std::size_t count = 0;
            if (!heldModule(f.type, base, count) || s_->program->module(base) == nullptr || count == 0)
                continue;
            const std::size_t each = f.size / count;
            for (std::size_t k = 0; k < count; ++k)
                setDefaults(static_cast<char*>(instance) + f.offset + k * each, base, depth + 1);
        }
    }

    void Patch::reset(float sampleRate)
    {
        rate_ = sampleRate > 0.0f ? sampleRate : 48000.0f;
        for (std::size_t i = 0; i < instances_.size(); ++i)
        {
            void* p = instances_[i];
            if (initialised_[i] && s_->end != nullptr)
                s_->end(p);
            std::memset(p, 0, instanceSize_);
            setDefaults(p, kModule, 0);
            for (std::size_t c = 0; c < params_.size(); ++c)   // the dials at their defaults until the Engine sets them
                if (params_[c] != nullptr)
                    fminus::Program::setValue(p, *static_cast<const fminus::Program::Parameter*>(params_[c]),
                                              static_cast<double>(controls_[c].def));
            if (s_->init != nullptr)
                s_->init(p, rate_);
            initialised_[i] = true;
        }
    }

    void Patch::setValue(int c, float plain, int i) noexcept
    {
        if (c < 0 || c >= kMaxControls || params_.empty() || params_[static_cast<std::size_t>(c)] == nullptr)
            return;
        const auto& param = *static_cast<const fminus::Program::Parameter*>(params_[static_cast<std::size_t>(c)]);
        if (i >= 0)
        {
            if (i < static_cast<int>(instances_.size()))
                fminus::Program::setValue(instances_[static_cast<std::size_t>(i)], param, static_cast<double>(plain));
            return;
        }
        for (void* p : instances_)
            fminus::Program::setValue(p, param, static_cast<double>(plain));
    }

    void Patch::setStream(int c, const float* samples, int i) noexcept
    {
        if (c < 0 || c >= kMaxControls || params_.empty() || params_[static_cast<std::size_t>(c)] == nullptr
            || controls_[static_cast<std::size_t>(c)].type != ControlType::real)
            return;
        const auto& param = *static_cast<const fminus::Program::Parameter*>(params_[static_cast<std::size_t>(c)]);
        if (i >= 0)
        {
            if (i < static_cast<int>(instances_.size()))
                fminus::Program::setStream(instances_[static_cast<std::size_t>(i)], param, samples);
            return;
        }
        for (void* p : instances_)
            fminus::Program::setStream(p, param, samples);
    }

    void Patch::process(float* left, float* right, int n) noexcept
    {
        if (s_->process != nullptr && !instances_.empty())
            s_->process(instances_[0], left, right, n);
    }

    void Patch::start(int i, float note, float velocity) noexcept
    {
        if (s_->start != nullptr && i >= 0 && i < static_cast<int>(instances_.size()))
            s_->start(instances_[static_cast<std::size_t>(i)], note, velocity);
    }

    void Patch::stop(int i) noexcept
    {
        if (s_->stop != nullptr && i >= 0 && i < static_cast<int>(instances_.size()))
            s_->stop(instances_[static_cast<std::size_t>(i)]);
    }

    bool Patch::render(int i, float* left, float* right, int n) noexcept
    {
        if (s_->render == nullptr || i < 0 || i >= static_cast<int>(instances_.size()))
            return false;
        return s_->render(instances_[static_cast<std::size_t>(i)], left, right, n);
    }
}
