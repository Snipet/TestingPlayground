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

#include "fminus/GuiSpec.h"

#include "Product.h"

#include "fminus/FMinus.h"

#include "fsdsp/core/ScopedFtz.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>

namespace playground
{
    namespace
    {
        constexpr bool kInstrument = product::kIsInstrument;
        constexpr const char* kModule = kInstrument ? "Voice" : "Effect";

        // One of the plugin's calls into the program: the method, whether the plugin needs it, how the plugin calls
        // it (as the author would declare it), the line appended to the program for it, and its module (null: the
        // main one).
        struct Call
        {
            const char* method;
            bool required;
            const char* declaration;
            const char* shim;
            const char* module = nullptr;
        };

        // The module Shared's, when the program has one (Patch.h).
        constexpr const char* kSharedModule = "Shared";

        // The module Host (Patch.h): host.fm, the plugin's own, read when a program #includes it and has no file of
        // that name.
        constexpr const char* kHostModule = "Host";
        constexpr const char* kHostFileName = "host.fm";
        constexpr const char* kHostFile = R"FM(// host.fm: what the host and the player are doing, for a program to read. The plugin provides this file: #include it.
//
// Declare a field `Host* host;` in the main module (each Voice, or the Effect; Shared may have one too) and the plugin
// points it at the one Host, which it fills before every call of start, stop and process. Read it; never write it.
//
//   host->bpm          the tempo, beats a minute (the host's; 120 when it says none)
//   host->beat         the song position at this call's first sample, in quarter notes; sample i of the call is at
//                      host->beat + i * host->bpm / (60 * rate). It moves while the host plays (when the host says
//                      nothing, as the Standalone, it runs on at 120 BPM from when the program started)
//   host->barStart     where the bar the song is in began, in quarter notes (host->beat - host->barStart: the beats
//                      into it)
//   host->beatsPerBar, host->beatUnit   the time signature (4 and 4)
//   host->playing      the host's transport runs
//   host->modwheel     the mod wheel (MIDI CC 1), 0 to 1
//   host->pitchbend    the pitch wheel, -1 to 1 (0 at rest): the program chooses its range (2 semitones is usual)
//   host->aftertouch   channel pressure, 0 to 1
//   host->sustain      the sustain pedal is down (the plugin holds the notes itself: a key let go while it is down
//                      gets its stop when the pedal comes up)
//
// A synced LFO: its rate host->bpm / 60 / beats (beats a cycle: 1 a quarter note, 4 a bar of 4/4, 0.5 an eighth), or,
// to follow the song (the same phase at the same place, whenever it is played), its phase from the position:
// phase = fract((host->beat + i * host->bpm / (60 * rate)) / beats).
module Host
{
    float bpm;
    float beat;
    float barStart;
    int beatsPerBar;
    int beatUnit;
    bool playing;
    float modwheel;
    float pitchbend;
    float aftertouch;
    bool sustain;
};
)FM";
        constexpr Call kSharedCalls[] = {
            { "init", false, "void init(float rate)",
              "void playground_shared_init(Shared* self, float rate) { self->init(rate); }", kSharedModule },
            { "end", false, "void end()", "void playground_shared_end(Shared* self) { self->end(); }", kSharedModule },
        };

        // A field's type as FMinus writes it ("Shared *") without its spaces.
        std::string squeezed(const std::string& type)
        {
            std::string s;
            for (const char ch : type)
                if (ch != ' ')
                    s += ch;
            return s;
        }

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
                    out += prefix + " " + kind + ": " + (c.module != nullptr ? c.module : kModule) + "::" + c.method + ": ";
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

    struct Patch::Compiled
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
        Init* sharedInit = nullptr;                // the module Shared's
        Plain* sharedEnd = nullptr;
    };

    Patch::Patch() : s_(std::make_unique<Compiled>()) {}

    Patch::Frozen::Frozen() : code_(std::make_unique<Compiled>()) {}
    Patch::Frozen::~Frozen() = default;

    Patch::~Patch()
    {
        freeInstances();
    }

    void Patch::endInstances() noexcept
    {
        const fsdsp::ScopedFtz ftz;                  // end runs in the mode the program is compiled for
        for (std::size_t i = 0; i < instances_.size(); ++i)
        {
            if (initialised_[i] && s_->end != nullptr)
                s_->end(instances_[i]);
            initialised_[i] = false;
        }
        if (sharedInitialised_ && s_->sharedEnd != nullptr)
            s_->sharedEnd(shared_);
        sharedInitialised_ = false;
    }

    void Patch::freeInstances() noexcept
    {
        endInstances();
        for (void* p : instances_)
            std::free(p);
        instances_.clear();
        initialised_.clear();
        std::free(shared_);
        shared_ = nullptr;
        std::free(host_);
        host_ = nullptr;
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
            if (name == kHostFileName)
                return fminus::Program::IncludedFile{ kHostFileName, kHostFile };
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
        // The module Shared's methods, when it has any.
        const fminus::Program::Module* sharedDecl = first->module(kSharedModule);
        bool sharedInit = false, sharedEnd = false;
        if (sharedDecl != nullptr)
            for (const Call& c : kSharedCalls)
                if (std::find(sharedDecl->methods.begin(), sharedDecl->methods.end(),
                              std::string(kSharedModule) + "::" + c.method) != sharedDecl->methods.end())
                {
                    shims.push_back(&c);
                    (std::strcmp(c.method, "init") == 0 ? sharedInit : sharedEnd) = true;
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
        Compiled& s = *patch->s_;
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
        const auto lookShared = [&](const char* name, auto*& fn) {
            std::string missing;
            fn = program.function<std::remove_pointer_t<std::remove_reference_t<decltype(fn)>>>(name, missing);
            r.errors += missing;
        };
        if (sharedInit)
            lookShared("playground_shared_init", s.sharedInit);
        if (sharedEnd)
            lookShared("playground_shared_end", s.sharedEnd);
        if (!r.errors.empty() || s.module == nullptr)
        {
            r.compileMs = msSince(t0);
            return r;
        }
        patch->params_.assign(static_cast<std::size_t>(kMaxControls), nullptr);
        for (std::size_t k = 0; k < names.size(); ++k)
        {
            const fminus::Program::Parameter* param = program.parameter(names[k]);
            patch->params_[k] = param;
            if (param == nullptr)
                continue;
            Slot& slot = patch->slots_[k];
            slot.stream = param->streamOffset;
            slot.value = param->valueOffset;
            slot.kind = param->type == "int" ? Slot::Kind::integer
                      : param->type == "bool" ? Slot::Kind::boolean : Slot::Kind::real;
        }
        {
            Recipe& rc = patch->recipe_;
            rc.files = files;
            rc.source = full;
            for (std::size_t k = 0; k < names.size() && k < rc.names.size(); ++k)
                rc.names[k] = names[k];
            rc.controls = patch->controls_;
            rc.size = s.module->size;
            rc.alignment = s.module->alignment;
            rc.start = s.start != nullptr;
            rc.stop = s.stop != nullptr;
            rc.render = s.render != nullptr;
            rc.process = s.process != nullptr;
        }

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
        // The module Shared's one instance, and the main module's fields that point at it.
        if (const fminus::Program::Module* m = program.module(kSharedModule))
        {
            patch->sharedAlign_ = std::max<std::size_t>(m->alignment, alignof(std::max_align_t));
            patch->sharedSize_ = (std::max<std::size_t>(m->size, 1) + patch->sharedAlign_ - 1) / patch->sharedAlign_
                               * patch->sharedAlign_;
            patch->shared_ = std::aligned_alloc(patch->sharedAlign_, patch->sharedSize_);
            if (patch->shared_ == nullptr)
            {
                r.errors = file + ": error: no memory for the module Shared\n";
                r.compileMs = msSince(t0);
                return r;
            }
            std::memset(patch->shared_, 0, patch->sharedSize_);
            for (const fminus::Program::Module::Field& f : s.module->fields)
                if (squeezed(f.type) == std::string(kSharedModule) + "*" && f.size == sizeof(void*))
                    patch->sharedFields_.push_back(f.offset);
            if (patch->sharedFields_.empty())
                r.notes += file + ": note: the plugin makes one module Shared for the program, but no field of " + kModule
                         + " points at it (`Shared* shared;`), so nothing reaches it\n";
        }
        // The module Host's one instance (host.fm), the fields that point at it (the main module's and Shared's), and
        // where its fields are.
        if (const fminus::Program::Module* m = program.module(kHostModule))
        {
            patch->hostAlign_ = std::max<std::size_t>(m->alignment, alignof(std::max_align_t));
            patch->hostSize_ = (std::max<std::size_t>(m->size, 1) + patch->hostAlign_ - 1) / patch->hostAlign_ * patch->hostAlign_;
            patch->host_ = std::aligned_alloc(patch->hostAlign_, patch->hostSize_);
            if (patch->host_ == nullptr)
            {
                r.errors = file + ": error: no memory for the module Host\n";
                r.compileMs = msSince(t0);
                return r;
            }
            std::memset(patch->host_, 0, patch->hostSize_);
            const std::string pointer = std::string(kHostModule) + "*";
            for (const fminus::Program::Module::Field& f : s.module->fields)
                if (squeezed(f.type) == pointer && f.size == sizeof(void*))
                    patch->hostFields_.push_back(f.offset);
            if (const fminus::Program::Module* sh = program.module(kSharedModule))
                for (const fminus::Program::Module::Field& f : sh->fields)
                    if (squeezed(f.type) == pointer && f.size == sizeof(void*))
                        patch->sharedHostFields_.push_back(f.offset);
            const auto at = [&](const char* name, const char* type, std::size_t& out) {
                for (const fminus::Program::Module::Field& f : m->fields)
                    if (f.name == name && squeezed(f.type) == type)
                    {
                        out = f.offset;
                        return;
                    }
            };
            HostLayout& l = patch->hostLayout_;
            at("bpm", "float", l.bpm);
            at("beat", "float", l.beat);
            at("barStart", "float", l.barStart);
            at("beatsPerBar", "int", l.beatsPerBar);
            at("beatUnit", "int", l.beatUnit);
            at("playing", "bool", l.playing);
            at("modwheel", "float", l.modwheel);
            at("pitchbend", "float", l.pitchbend);
            at("aftertouch", "float", l.aftertouch);
            at("sustain", "bool", l.sustain);
            for (const fminus::Program::Module::Field& f : m->fields)
                if (f.name == "playing" || f.name == "sustain")
                    l.boolSize = f.size;
            if (patch->hostFields_.empty() && patch->sharedHostFields_.empty())
                r.notes += file + ": note: the program has a module Host, but no field points at it (`Host* host;` in "
                         + kModule + "), so the plugin's tempo, position and wheels reach nothing\n";
            patch->setHost(HostBlock{});
        }
        patch->sources_ = files;
        // The panel, and a tap (where it sits in an instance) for each display of one of the main module's fields.
        std::vector<PanelField> fields;
        for (const fminus::Program::Module::Field& f : s.module->fields)
            fields.push_back({ f.name, f.type });
        // A .gui file (GuiSpec.h), the first one, makes the interface; else the #pragma panel lines do.
        const SourceFile* gui = nullptr;
        for (const SourceFile& f : files)
            if (isGuiFile(f.name))
            {
                if (gui == nullptr)
                    gui = &f;
                else
                    r.notes += f.name + ": note: the interface is " + gui->name + "'s (the first .gui file); this one is "
                                                                                 "left out\n";
            }
        if (gui != nullptr)
        {
            std::string ignored;
            const PanelSpec pragmas = readPanelSpec(compiled, patch->controls_, fields, kInstrument, ignored);
            if (!pragmas.sections.empty() || !pragmas.displays.empty())
                r.notes += file + ": note: the interface is " + gui->name + "'s: the #pragma panel lines are left out\n";
            std::vector<std::string> fileNames;
            for (const SourceFile& f : files)
                fileNames.push_back(f.name);
            patch->panel_ = readGuiSpec(*gui, patch->controls_, fields, kInstrument, fileNames, r.notes);
        }
        else
            patch->panel_ = readPanelSpec(compiled, patch->controls_, fields, kInstrument, r.notes);
        for (const PanelDisplay& d : patch->panel_.displays)
        {
            if (d.tap < 0)
                continue;
            for (const fminus::Program::Module::Field& f : s.module->fields)
                if (f.name == d.source)
                {
                    Tap t;
                    t.offset = f.offset;
                    t.type = f.type == "float" ? Tap::Type::real : (f.type == "int" ? Tap::Type::integer : Tap::Type::boolean);
                    patch->taps_.push_back(t);
                    break;
                }
        }
        patch->mapFields();
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
        const fsdsp::ScopedFtz ftz;                  // init (and end) run in the mode the program is compiled for
        rate_ = sampleRate > 0.0f ? sampleRate : 48000.0f;
        endInstances();
        // Shared first: the instances' init may read what its init made.
        if (shared_ != nullptr)
        {
            std::memset(shared_, 0, sharedSize_);
            setDefaults(shared_, kSharedModule, 0);
            for (const std::size_t at : sharedHostFields_)
                std::memcpy(static_cast<char*>(shared_) + at, &host_, sizeof host_);
            if (s_->sharedInit != nullptr)
                s_->sharedInit(shared_, rate_);
            sharedInitialised_ = true;
        }
        for (std::size_t i = 0; i < instances_.size(); ++i)
        {
            void* p = instances_[i];
            std::memset(p, 0, instanceSize_);
            setDefaults(p, kModule, 0);
            for (std::size_t c = 0; c < params_.size(); ++c)   // the dials at their defaults until the Engine sets them
                if (params_[c] != nullptr)
                    fminus::Program::setValue(p, *static_cast<const fminus::Program::Parameter*>(params_[c]),
                                              static_cast<double>(controls_[c].def));
            for (const std::size_t at : sharedFields_)
                std::memcpy(static_cast<char*>(p) + at, &shared_, sizeof shared_);
            for (const std::size_t at : hostFields_)
                std::memcpy(static_cast<char*>(p) + at, &host_, sizeof host_);
            if (s_->init != nullptr)
                s_->init(p, rate_);
            initialised_[i] = true;
        }
    }

    void Patch::setHost(const HostBlock& h) noexcept
    {
        if (host_ == nullptr)
            return;
        char* base = static_cast<char*>(host_);
        const HostLayout& l = hostLayout_;
        const auto put = [base](std::size_t at, const auto& v) {
            if (at != HostLayout::kNone)
                std::memcpy(base + at, &v, sizeof v);
        };
        const auto flag = [&](std::size_t at, bool v) {
            if (at == HostLayout::kNone)
                return;
            if (l.boolSize == 1)
            {
                const uint8_t b = v ? 1 : 0;
                std::memcpy(base + at, &b, 1);
            }
            else
            {
                const int32_t b = v ? 1 : 0;
                std::memcpy(base + at, &b, sizeof b);
            }
        };
        put(l.bpm, h.bpm);
        put(l.beat, h.beat);
        put(l.barStart, h.barStart);
        put(l.beatsPerBar, static_cast<int32_t>(h.beatsPerBar));
        put(l.beatUnit, static_cast<int32_t>(h.beatUnit));
        flag(l.playing, h.playing);
        put(l.modwheel, h.modwheel);
        put(l.pitchbend, h.pitchbend);
        put(l.aftertouch, h.aftertouch);
        flag(l.sustain, h.sustain);
    }

    std::string_view Patch::hostSource() noexcept { return kHostFile; }

    void Patch::process(float* left, float* right, int n) noexcept
    {
        const Compiled& c = code();
        if (c.process != nullptr && !instances_.empty())
            c.process(instances_[0], left, right, n);
    }

    void Patch::start(int i, float note, float velocity) noexcept
    {
        const Compiled& c = code();
        if (c.start != nullptr && i >= 0 && i < static_cast<int>(instances_.size()))
            c.start(instances_[static_cast<std::size_t>(i)], note, velocity);
    }

    void Patch::stop(int i) noexcept
    {
        const Compiled& c = code();
        if (c.stop != nullptr && i >= 0 && i < static_cast<int>(instances_.size()))
            c.stop(instances_[static_cast<std::size_t>(i)]);
    }

    bool Patch::render(int i, float* left, float* right, int n) noexcept
    {
        const Compiled& c = code();
        if (c.render == nullptr || i < 0 || i >= static_cast<int>(instances_.size()))
            return false;
        return c.render(instances_[static_cast<std::size_t>(i)], left, right, n);
    }

    // ---- frozen dials ---------------------------------------------------------------------------------------------------
    Patch::Recipe Patch::recipe() const
    {
        Recipe r = recipe_;
        r.program = program_;
        return r;
    }

    void Patch::useFrozen(const Frozen* f) noexcept
    {
        active_ = f != nullptr && f->program_ == program_ ? f->code_.get() : nullptr;
    }

    std::unique_ptr<Patch::Frozen> Patch::freeze(const Recipe& r, const std::array<float, kMaxControls>& plain,
                                                 std::string& why, bool exactMath)
    {
        const auto t0 = std::chrono::steady_clock::now();
        if (r.files.empty() || r.source.empty())
        {
            why = "no program";
            return nullptr;
        }
        const Sources& files = r.files;
        fminus::Program::Options build;
        build.fileName = files.front().name;
        build.include = [&files](std::string_view name, std::string_view) -> std::optional<fminus::Program::IncludedFile> {
            if (const SourceFile* f = findFile(files, name))
                return fminus::Program::IncludedFile{ f->name, f->text };
            if (name == kHostFileName)
                return fminus::Program::IncludedFile{ kHostFileName, kHostFile };
            return std::nullopt;
        };
        build.denormals = fminus::Program::Denormals::Flush;     // as compile()'s
        build.math = exactMath ? fminus::Program::Math::Vector   // dsp.freeze's measure of exact math
                               : fminus::Program::Math::Fast;    // the copy's math (Patch.h, "Frozen dials")
        for (std::size_t k = 0; k < r.names.size(); ++k)
            if (!r.names[k].empty() && r.controls[k].used)
                build.parameters[r.names[k]] = fminus::Program::Binding::frozen(static_cast<double>(plain[k]));
        auto f = std::unique_ptr<Frozen>(new Frozen());
        std::string errors;
        f->code_->program = fminus::Program::compile(r.source, errors, build);
        if (!f->code_->program)
        {
            why = errors.empty() ? std::string("it did not compile") : errors;
            return nullptr;
        }
        fminus::Program& program = *f->code_->program;
        Compiled& s = *f->code_;
        s.module = program.module(kModule);
        if (s.module == nullptr || s.module->size != r.size || s.module->alignment != r.alignment)
        {
            why = "its layout is not the program's";
            return nullptr;
        }
        std::string missing;
        const auto look = [&](bool has, const char* method, auto*& fn) {
            if (has)
                fn = program.function<std::remove_pointer_t<std::remove_reference_t<decltype(fn)>>>(
                    std::string("playground_") + method, missing);
        };
        if constexpr (kInstrument)
        {
            look(r.start, "start", s.start);
            look(r.render, "process", s.render);
            look(r.stop, "stop", s.stop);
        }
        else
        {
            look(r.process, "process", s.process);
        }
        if (!missing.empty())
        {
            why = missing;
            return nullptr;
        }
        f->program_ = r.program;
        f->values_ = plain;
        f->ms_ = msSince(t0);
        return f;
    }

    float Patch::tap(int t, int i) const noexcept
    {
        if (t < 0 || t >= static_cast<int>(taps_.size()) || i < 0 || i >= static_cast<int>(instances_.size()))
            return 0.0f;
        const Tap& k = taps_[static_cast<std::size_t>(t)];
        const char* at = static_cast<const char*>(instances_[static_cast<std::size_t>(i)]) + k.offset;
        switch (k.type)
        {
            case Tap::Type::real:
            {
                float v = 0.0f;
                std::memcpy(&v, at, sizeof v);
                return std::isfinite(v) ? v : 0.0f;
            }
            case Tap::Type::integer:
            {
                int32_t v = 0;
                std::memcpy(&v, at, sizeof v);
                return static_cast<float>(v);
            }
            case Tap::Type::boolean:
                return *at != 0 ? 1.0f : 0.0f;
        }
        return 0.0f;
    }

    // ---- the fields (dsp/Debug.h) --------------------------------------------------------------------------------------
    namespace
    {
        constexpr std::size_t kMaxMapped = 4096;       // fields in a map at most
        constexpr std::size_t kMaxElements = 16;       // an array's elements expanded at most
        constexpr const char* kLanes[4] = { "x", "y", "z", "w" };   // a float2's or float4's lanes

        struct Mapper
        {
            const fminus::Program& program;
            std::vector<Patch::Field>& out;
            std::vector<std::string>& left;
            bool full = false;

            void leave(const std::string& line)
            {
                if (left.size() < 64)
                    left.push_back(line);
            }

            void add(Patch::Field f)
            {
                if (out.size() >= kMaxMapped)
                {
                    if (!full)
                        leave("the rest: the map keeps " + std::to_string(kMaxMapped) + " fields");
                    full = true;
                    return;
                }
                out.push_back(std::move(f));
            }

            // One element of type `type` (no array) at `offset`, of `size` bytes, as `path`.
            void element(const std::string& type, const std::string& path, std::size_t offset, std::size_t size,
                         const std::string& module, const std::string& name, Patch::Field::Home home, int depth)
            {
                Patch::Field f;
                f.path = path;
                f.module = module;
                f.name = name;
                f.offset = offset;
                f.home = home;
                if (type == "float" || type == "int" || type == "bool")
                {
                    f.type = type == "float" ? Patch::Field::Type::real
                           : type == "int"   ? Patch::Field::Type::integer : Patch::Field::Type::boolean;
                    add(std::move(f));
                    return;
                }
                if (type == "float2" || type == "float4" || type == "float8" || type == "poly float")
                {
                    const std::size_t lanes = size / sizeof(float);
                    for (std::size_t l = 0; l < lanes; ++l)
                    {
                        Patch::Field lane = f;
                        lane.path = path + "." + (lanes <= 4 ? std::string(kLanes[l]) : "s" + std::to_string(l));
                        lane.offset = offset + l * sizeof(float);
                        lane.type = Patch::Field::Type::real;
                        add(std::move(lane));
                    }
                    return;
                }
                if (const fminus::Program::Module* m = program.module(type); m != nullptr && depth < 16)
                {
                    fields(*m, path + ".", offset, home, depth + 1);
                    return;
                }
                leave(path + ": " + type + ", not a number the trace reads");
            }

            // A module's fields from `base`, their paths after `prefix`.
            void fields(const fminus::Program::Module& m, const std::string& prefix, std::size_t base, Patch::Field::Home home,
                        int depth)
            {
                for (const fminus::Program::Module::Field& f : m.fields)
                {
                    const std::string path = prefix + f.name;
                    if (f.type.rfind("param ", 0) == 0)
                        continue;                      // a dial (or a held module's parameter at its default)
                    if (f.type.find('*') != std::string::npos)
                    {
                        const std::string t = squeezed(f.type);
                        if (!((t == "Shared*" || t == "Host*") && depth == 0))
                            leave(path + ": " + f.type + ", a pointer, not followed");
                        continue;                      // Shared* and Host*: mapFields maps what they point at
                    }
                    const std::size_t open = f.type.find('[');
                    std::string type = f.type.substr(0, open);
                    while (!type.empty() && type.back() == ' ')
                        type.pop_back();
                    std::size_t count = 1;
                    for (std::size_t at = open; at != std::string::npos; at = f.type.find('[', at + 1))
                        count *= static_cast<std::size_t>(std::max(1L, std::strtol(f.type.c_str() + at + 1, nullptr, 10)));
                    if (open == std::string::npos)
                    {
                        element(type, path, base + f.offset, f.size, m.name, f.name, home, depth);
                        continue;
                    }
                    if (count > kMaxElements)
                    {
                        leave(path + ": " + f.type + ", more than " + std::to_string(kMaxElements) + " elements");
                        continue;
                    }
                    const std::size_t each = f.size / count;
                    for (std::size_t k = 0; k < count; ++k)
                        element(type, path + "[" + std::to_string(k) + "]", base + f.offset + k * each, each, m.name, f.name,
                                home, depth);
                }
            }
        };
    }

    void Patch::mapFields()
    {
        fields_.clear();
        unmapped_.clear();
        const fminus::Program& program = *s_->program;
        Mapper map{ program, fields_, unmapped_, false };
        const fminus::Program::Module* main = program.module(kModule);
        if (main == nullptr)
            return;
        map.fields(*main, "", 0, Field::Home::instance, 0);
        // What the main module's Shared* and Host* point at, through the first of each (and Host's through Shared's).
        std::string viaShared, viaHost;
        for (const fminus::Program::Module::Field& f : main->fields)
        {
            const std::string t = squeezed(f.type);
            if (t == std::string(kSharedModule) + "*" && viaShared.empty())
                viaShared = f.name;
            if (t == std::string(kHostModule) + "*" && viaHost.empty())
                viaHost = f.name;
        }
        const fminus::Program::Module* shared = program.module(kSharedModule);
        if (shared != nullptr && shared_ != nullptr && !viaShared.empty())
        {
            map.fields(*shared, viaShared + "->", 0, Field::Home::shared, 0);
            if (viaHost.empty())
                for (const fminus::Program::Module::Field& f : shared->fields)
                    if (squeezed(f.type) == std::string(kHostModule) + "*")
                    {
                        viaHost = viaShared + "->" + f.name;
                        break;
                    }
        }
        if (const fminus::Program::Module* host = program.module(kHostModule); host != nullptr && host_ != nullptr && !viaHost.empty())
            map.fields(*host, viaHost + "->", 0, Field::Home::host, 0);
    }

    const void* Patch::instance(int i) const noexcept
    {
        return i >= 0 && i < static_cast<int>(instances_.size()) ? instances_[static_cast<std::size_t>(i)] : nullptr;
    }

    double Patch::read(const Field& f, int i) const noexcept
    {
        const void* home = f.home == Field::Home::shared ? shared_ : f.home == Field::Home::host ? host_ : instance(i);
        if (home == nullptr)
            return 0.0;
        const char* at = static_cast<const char*>(home) + f.offset;
        switch (f.type)
        {
            case Field::Type::real:
            {
                float v = 0.0f;
                std::memcpy(&v, at, sizeof v);
                return static_cast<double>(v);
            }
            case Field::Type::integer:
            {
                int32_t v = 0;
                std::memcpy(&v, at, sizeof v);
                return static_cast<double>(v);
            }
            case Field::Type::boolean:
                return *at != 0 ? 1.0 : 0.0;
        }
        return 0.0;
    }

    // The front of compile()'s pass 1, kept in step with it: FMinus over the program as written (its messages), then the
    // main module and its required methods. Nothing appended, no second pass, no instance.
    Patch::Result Patch::check(const Sources& files)
    {
        const auto t0 = std::chrono::steady_clock::now();
        Result r;
        if (files.empty())
        {
            r.errors = std::string(kProgramName) + ": error: there is no program\n";
            return r;
        }
        const std::string file = files.front().name;
        const fminus::Program::IncludeResolver include =
            [&files](std::string_view name, std::string_view) -> std::optional<fminus::Program::IncludedFile> {
            if (const SourceFile* f = findFile(files, name))
                return fminus::Program::IncludedFile{ f->name, f->text };
            if (name == kHostFileName)
                return fminus::Program::IncludedFile{ kHostFileName, kHostFile };
            return std::nullopt;
        };
        fminus::Program::Options read;
        read.optimisation = 0;
        read.knobVersions = 0;
        read.fileName = file;
        read.include = include;
        const std::unique_ptr<fminus::Program> first = fminus::Program::compile(files.front().text, r.errors, read);
        if (first)
        {
            const fminus::Program::Module* main = first->module(kModule);
            if (main == nullptr)
                r.errors = file + ": error: the plugin plays a module " + kModule + " (README.md, \"Writing a program\")\n";
            else
                for (std::size_t k = 0; k < callCount(); ++k)
                {
                    const Call& c = calls()[k];
                    const std::string name = std::string(kModule) + "::" + c.method;
                    if (c.required && std::find(main->methods.begin(), main->methods.end(), name) == main->methods.end())
                        r.errors += file + ": error: " + kModule + " needs a method `" + c.declaration + "`" +
                                    (audioThread(c) ? ", which the plugin calls on the audio thread" : "") + "\n";
                }
        }
        r.compileMs = msSince(t0);
        return r;
    }
}
