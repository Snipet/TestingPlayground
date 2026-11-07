// Source/fminus/Patch.cpp: see Patch.h.
//
// compile() runs FMinus twice. The first pass, at -O0, reads the program as it is: does it compile, has it the main
// module and its methods, what are its parameters? Only then can the second choose the parameters' bindings (a float
// a stream), and append the plugin's calls (one function a method, one line each, after the program's last line), at
// -O2, flushing subnormals as the audio thread does (Program::Options::denormals). FMinus reports a mistake in how a
// method is declared as an error in one of those lines, which the program's author never wrote: such a message is
// rewritten to say which method the plugin calls, and how. Both passes read the program's other files through
// Program::Options::include, by name: an #include of a name no file has is FMinus's "'name' file not found". A
// netlist's (a .cir file's) #include reads the module generated from it (fminus/Circuits.h): the compile's Generator
// generates it when the first pass's #include first reaches it, and the second pass reads it again.
#include "fminus/Patch.h"

#include "fminus/GuiSpec.h"

#include "Product.h"

#include "fminus/FMinus.h"

#include "fsdsp/core/ScopedFtz.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#include <malloc.h>                                  // _aligned_malloc
#endif
#include <cstring>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>

namespace playground
{
    namespace
    {
        // Aligned memory for a program's instances: C++17's aligned_alloc, which Windows' C runtime lacks (its
        // _aligned_malloc, freed with _aligned_free). The size is always a multiple of the alignment here.
        void* alignedAlloc(std::size_t alignment, std::size_t size) noexcept
        {
#if defined(_WIN32)
            return _aligned_malloc(size, alignment);
#else
            return std::aligned_alloc(alignment, size);
#endif
        }

        void alignedFree(void* p) noexcept
        {
#if defined(_WIN32)
            _aligned_free(p);
#else
            std::free(p);
#endif
        }

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
        constexpr const char* kHostFileName = "host.fm";   // its text: HostFile.cpp (Patch::hostSource)
        constexpr Call kSharedCalls[] = {
            { "init", false, "void init(float rate)",
              "void playground_shared_init(Shared* self, float rate) { self->init(rate); }", kSharedModule },
            { "end", false, "void end()", "void playground_shared_end(Shared* self) { self->end(); }", kSharedModule },
        };
        // An instrument's Shared hooks (Patch.h, "Shared on its own"), in lang::sharedHooks' order: each appended only
        // when the program declares it as here.
        constexpr Call kSharedHookCalls[] = {
            { "tick", false, "void tick(int n)",
              "void playground_shared_tick(Shared* self, int n) [[clang::nonblocking]] { self->tick(n); }", kSharedModule },
            { "noteOn", false, "void noteOn(float note, float velocity)",
              "void playground_shared_note_on(Shared* self, float note, float velocity) [[clang::nonblocking]] "
              "{ self->noteOn(note, velocity); }", kSharedModule },
            { "noteOff", false, "void noteOff(float note)",
              "void playground_shared_note_off(Shared* self, float note) [[clang::nonblocking]] { self->noteOff(note); }",
              kSharedModule },
        };
        constexpr const char* kSharedHookShims[] = { "playground_shared_tick", "playground_shared_note_on",
                                                     "playground_shared_note_off" };
        // A mono instrument's Voice::legato (Patch.h, "Mono"): appended only to a mono program, and only when the program
        // declares it as here.
        constexpr Call kLegatoCall = { "legato", false, "void legato(float note, float velocity)",
                                       "void playground_legato(Voice* self, float note, float velocity) [[clang::nonblocking]] "
                                       "{ self->legato(note, velocity); }" };

        // A #pragma mono line (Patch.h, "Mono"): its words after "mono" (a // comment ends them) into `words`; false for
        // any other line.
        bool monoLine(std::string_view line, std::vector<std::string>& words)
        {
            const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
            const auto nameChar = [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            };
            std::size_t i = 0;
            const auto skip = [&] {
                while (i < line.size() && space(line[i]))
                    ++i;
            };
            const auto word = [&](std::string_view w) {
                if (line.substr(i, w.size()) != w || (i + w.size() < line.size() && nameChar(line[i + w.size()])))
                    return false;
                i += w.size();
                return true;
            };
            skip();
            if (i >= line.size() || line[i] != '#')
                return false;
            ++i;
            skip();
            if (!word("pragma"))
                return false;
            skip();
            if (!word("mono"))
                return false;
            words.clear();
            while (true)
            {
                skip();
                if (i >= line.size() || line.compare(i, 2, "//") == 0)
                    return true;
                const std::size_t from = i;
                while (i < line.size() && !space(line[i]) && line.compare(i, 2, "//") != 0)
                    ++i;
                words.emplace_back(line.substr(from, i - from));
            }
        }

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
        static_assert(countOf(kSharedHookCalls) == static_cast<std::size_t>(lang::kSharedHooks)
                      && std::size(kSharedHookShims) == static_cast<std::size_t>(lang::kSharedHooks));

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
        // firstShim + k, then the circuit dials' shims, circuits[j] on the line after those) said in the author's terms:
        // the method (or the field), how the plugin calls it, and FMinus's reason.
        std::string translate(const std::string& errors, std::string_view fileName, int firstShim,
                              const std::vector<const Call*>& shims, const std::vector<std::string>& circuits = {})
        {
            std::istringstream in(errors);
            std::string out, line;
            const std::string prefix = std::string(fileName) + ":";
            while (std::getline(in, line))
            {
                int ln = 0, col = 0, used = 0;
                const int shimCount = static_cast<int>(shims.size());
                if (line.rfind(prefix, 0) == 0
                    && std::sscanf(line.c_str() + prefix.size(), "%d:%d: %n", &ln, &col, &used) >= 2 && used > 0
                    && ln >= firstShim + shimCount && ln - firstShim - shimCount < static_cast<int>(circuits.size()))
                {
                    std::string rest = line.substr(prefix.size() + static_cast<std::size_t>(used));
                    const std::size_t colon = rest.find(": ");
                    const std::string kind = colon == std::string::npos ? "error" : rest.substr(0, colon);
                    const std::string why = colon == std::string::npos ? rest : rest.substr(colon + 2);
                    out += prefix + " " + kind + ": " + kModule + "::" + circuits[static_cast<std::size_t>(ln - firstShim - shimCount)]
                         + " (its dials, on the audio thread): " + why + "\n";
                    continue;
                }
                if (line.rfind(prefix, 0) == 0
                    && std::sscanf(line.c_str() + prefix.size(), "%d:%d: %n", &ln, &col, &used) >= 2 && used > 0
                    && ln >= firstShim && ln - firstShim < shimCount)
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

        bool sameFolded(std::string_view a, std::string_view b) noexcept
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
                   });
        }

        // The circuit dials of `files` (Patch.h, "Circuits": lang::circuitDials), each binding checked against what
        // pass 1 compiled: the main module's field of that name holds that netlist's module, with the extent Lang read
        // (FMinus's type), the generated set() takes the pots and switches Lang read, in order (the Generator's shape),
        // and no dial's name is a parameter's. One that differs gets no dials and a note; then the slots after the
        // first `taken` (lang::assignSlots), and a note for each dial past kMaxControls.
        std::vector<lang::CircuitBinding> checkedBindings(const Sources& files, const fminus::Program::Module& main,
                                                          const circuits::Generator& gen, const std::string& file, int taken,
                                                          std::string& notes)
        {
            Sources read;
            for (const SourceFile& f : files)
                if (!isGuiFile(f.name))
                    read.push_back(f);
            lang::Service service;
            lang::Options o;
            o.instrument = kInstrument;
            service.setOptions(o);
            service.setFiles(read);
            lang::CircuitDials d = lang::circuitDials(service, kInstrument);
            for (lang::CircuitBinding& b : d.bindings)
            {
                if (b.own || b.names.empty())
                    continue;
                std::string why;
                const auto f = std::find_if(main.fields.begin(), main.fields.end(),
                                            [&](const fminus::Program::Module::Field& x) { return x.name == b.field; });
                std::string base;
                std::size_t count = 0;
                const circuits::Generator::Shape* shape = gen.shape(b.file);
                if (f == main.fields.end())
                    why = std::string(kModule) + " has no field " + b.field + " as FMinus compiled it";
                else if (!heldModule(f->type, base, count) || base != b.module)
                    why = "FMinus compiled it as " + f->type;
                else if (b.count() <= 0 || count != static_cast<std::size_t>(b.count()))
                    why = "its extent is not one the playground reads (" + f->type + ")";
                else if (shape == nullptr || shape->module != b.module)
                    why = b.file + " generated no module " + b.module;
                else
                {
                    std::vector<std::string> pots, switches;
                    for (const lang::CircuitPot& p : b.pots)
                        pots.push_back(p.name);
                    for (const lang::CircuitSwitch& w : b.switches)
                        switches.push_back(w.name);
                    if (pots != shape->pots || switches != shape->switches)
                        why = "its set() is not what the playground read of " + b.file;
                }
                for (const std::string& n : b.names)
                    for (const fminus::Program::Parameter& p : main.parameters)
                        if (why.empty() && sameFolded(n, p.name))
                            why = "its dial " + n + " would be the parameter " + p.name + "'s name";
                if (why.empty())
                    continue;
                notes += file + ": note: " + b.field + ": " + why + ", so " + b.module + "'s pots have no dials\n";
                b.names.clear();
            }
            lang::assignSlots(d, taken);
            for (const lang::CircuitBinding& b : d.bindings)
                if (!b.names.empty())
                    for (int a = 0; a < b.args(); ++a)
                        if (b.slots[static_cast<std::size_t>(a)] < 0)
                            notes += file + ": note: " + b.field + "'s " + (a < static_cast<int>(b.pots.size()) ? "pot " : "switch ")
                                   + b.names[static_cast<std::size_t>(a)] + " has no dial (the plugin has "
                                   + std::to_string(kMaxControls) + ": the host's " + std::to_string(kHostControls)
                                   + " and the panel's " + std::to_string(kPanelControls) + "): it stays at its default\n";
            return d.bindings;
        }

        // A bound field's shim (Patch.h, "Circuits"): one line, every element's set() with the dials' values, a switch's
        // position cast to int.
        std::string circuitShim(std::size_t k, const lang::CircuitBinding& b)
        {
            std::string args;
            for (int a = 0; a < b.args(); ++a)
                args += std::string(a == 0 ? "" : ", ") + (a < static_cast<int>(b.pots.size()) ? "" : "(int)") + "v["
                      + std::to_string(a) + "]";
            std::string loops, index;
            for (std::size_t d = 0; d < b.dims.size(); ++d)
            {
                const std::string i = "i" + std::to_string(d);
                loops += "for (int " + i + " = 0; " + i + " < " + std::to_string(b.dims[d]) + "; ++" + i + ") ";
                index += "[" + i + "]";
            }
            return std::string("void playground_circuit_") + std::to_string(k) + "(" + kModule + "* self, float* restrict v) "
                   "[[clang::nonblocking]] { " + loops + "self->" + b.field + index + ".set(" + args + "); }";
        }

        // How a message on a bound field's shim names it: "amp: the plugin calls amp[k].set(drive, tone, level)".
        std::string circuitWords(const lang::CircuitBinding& b)
        {
            std::string args;
            for (const std::string& n : b.names)
                args += (args.empty() ? "" : ", ") + n;
            return b.field + ": the plugin calls " + b.field + (b.dims.empty() ? "" : "[k]") + ".set(" + args + ")";
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

        // The program's files to FMinus's #include, by name: a file's text; a netlist's (.cir), the module `circuits`
        // has for it (its "x.cir.fm": the Generator of compile() and check() generates it the first time, freeze()'s,
        // ir()'s and frozenIr()'s serves the recipe's); and host.fm where the program has no file of that name.
        // `read`, when given, gets each FMinus file read, in the order first read (the panel's lines come from them).
        // It reads `files` and `circuits` while FMinus compiles.
        fminus::Program::IncludeResolver includeOf(const Sources& files, circuits::Generator& circuits,
                                                   std::vector<const SourceFile*>* read = nullptr)
        {
            return [&files, &circuits, read](std::string_view name,
                                             std::string_view) -> std::optional<fminus::Program::IncludedFile> {
                if (const SourceFile* f = findFile(files, name))
                {
                    if (isCircuitFile(f->name))
                    {
                        const std::string* text = circuits.module(f->name);
                        if (text == nullptr)
                            return std::nullopt;
                        return fminus::Program::IncludedFile{ f->name + ".fm", *text };
                    }
                    if (read != nullptr && std::find(read->begin(), read->end(), f) == read->end())
                        read->push_back(f);
                    return fminus::Program::IncludedFile{ f->name, f->text };
                }
                if (name == kHostFileName)
                    return fminus::Program::IncludedFile{ kHostFileName, std::string(Patch::hostSource()) };
                return std::nullopt;
            };
        }

        // The plugin's error for a program whose first file is a netlist: the program is FMinus, which #includes it.
        std::string netlistFirst(const std::string& file)
        {
            return file + ": error: the program is an FMinus file; #include the netlist (#include \"" + file
                 + "\" in a program.fm before it)\n";
        }

        // The plugin's macros (Lang.h's kDefines), as every compile here defines them (Patch.h, "Conditionals and
        // warnings").
        std::map<std::string, std::string> defines()
        {
            std::map<std::string, std::string> out;
            for (const auto& [name, value] : lang::hostDefines())
                out[name] = value;
            return out;
        }

        // A program's warnings (FMinus's Program::warnings, a netlist's module's cited at the netlist) as the plugin
        // shows them: a warning a line, without clang's note after it at its own place ("add missing switch cases"),
        // which would mark the same word again.
        std::string warningsOf(const fminus::Program& program, const circuits::Generator& circuits)
        {
            const std::string all = circuits.recite(program.warnings());
            std::string out, lastAt;
            std::size_t from = 0;
            while (from < all.size())
            {
                std::size_t end = all.find('\n', from);
                if (end == std::string::npos)
                    end = all.size();
                const std::string line = all.substr(from, end - from);
                from = end + 1;
                const std::size_t w = line.find(": warning: "), n = line.find(": note: ");
                if (n != std::string::npos && !lastAt.empty() && line.compare(0, n, lastAt) == 0)
                    continue;
                lastAt = w != std::string::npos ? line.substr(0, w) : std::string();
                if (!line.empty())
                    out += line + "\n";
            }
            return out;
        }

        // The files FMinus read (`compiled`, of `files`) as it read them: the lines of a group a conditional skipped
        // emptied, so that the panel's #pragma lines are read only where FMinus reads the program.
        std::vector<SourceFile> asRead(const std::vector<const SourceFile*>& compiled, const Sources& files)
        {
            const std::vector<lang::FileConditionals> cond = lang::conditionals(files, lang::hostDefines());
            std::vector<SourceFile> out;
            for (const SourceFile* f : compiled)
            {
                SourceFile copy = *f;
                const std::size_t k = static_cast<std::size_t>(f - files.data());
                if (k < cond.size() && std::find(cond[k].skipped.begin(), cond[k].skipped.end(), 1) != cond[k].skipped.end())
                {
                    std::string text;
                    std::size_t line = 0, from = 0;
                    while (from <= f->text.size())
                    {
                        const std::size_t end = std::min(f->text.find('\n', from), f->text.size());
                        if (line >= cond[k].skipped.size() || cond[k].skipped[line] == 0)
                            text.append(f->text, from, end - from);
                        if (end < f->text.size())
                            text += '\n';
                        from = end + 1;
                        ++line;
                    }
                    copy.text = std::move(text);
                }
                out.push_back(std::move(copy));
            }
            return out;
        }

        // compile()'s second pass's options but for the #include: the audio thread's denormal mode, and each dial a
        // stream (a float) or a knob. ir() gives FMinus the same, so its text is the one the program's code came from.
        fminus::Program::Options playOptions(const std::string& fileName, const std::array<std::string, kMaxControls>& names,
                                             const std::array<bool, kMaxControls>& streams, bool loopBudget, bool boundsCheck)
        {
            fminus::Program::Options o;
            o.fileName = fileName;
            o.defines = defines();
            o.denormals = fminus::Program::Denormals::Flush;     // the audio thread's mode (ScopedNoDenormals)
            o.loopBudget = loopBudget;                           // Patch.h, "The loop budget"
            o.boundsCheck = boundsCheck;                         // Patch.h, "Bounds checks"
            for (std::size_t k = 0; k < names.size(); ++k)
                if (!names[k].empty() && streams[k])
                    o.parameters[names[k]] = fminus::Program::Binding::stream();
            return o;
        }

        // freeze()'s (and frozenIr()'s) options but for the #include: compile()'s mode, the copy's math, every dial the
        // program has frozen at its plain value.
        fminus::Program::Options frozenOptions(const Patch::Recipe& r, const std::array<float, kMaxControls>& plain,
                                               bool exactMath)
        {
            fminus::Program::Options o;
            o.fileName = r.files.front().name;
            o.defines = defines();                               // as compile()'s: the program's own fields and code
            o.denormals = fminus::Program::Denormals::Flush;     // as compile()'s
            o.loopBudget = r.loopBudget;                         // as compile()'s: the copy keeps the checks
            o.boundsCheck = false;                               // the build that plays at full speed (Patch.h,
                                                                 // "Bounds checks"): its code as without the option
            o.math = exactMath ? fminus::Program::Math::Vector   // dsp.freeze's measure of exact math
                               : fminus::Program::Math::Fast;    // the copy's math (Patch.h, "Frozen dials")
            for (std::size_t k = 0; k < r.names.size(); ++k)
                if (!r.names[k].empty() && r.controls[k].used)
                    o.parameters[r.names[k]] = fminus::Program::Binding::frozen(static_cast<double>(plain[k]));
            return o;
        }
    }

    struct Patch::Compiled
    {
        using Process = void(void*, float*, float*, int);
        using Render = bool(void*, float*, float*, int);
        using Start = void(void*, float, float);
        using Init = void(void*, float);
        using Plain = void(void*);
        using Circuit = void(void*, float*);           // a circuit dial's shim (Patch.h, "Circuits")

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
        using Tick = void(void*, int);
        using NoteOff = void(void*, float);
        Tick* sharedTick = nullptr;                // an instrument's Shared hooks (Patch.h, "Shared on its own")
        Start* sharedNoteOn = nullptr;
        NoteOff* sharedNoteOff = nullptr;
        Start* legato = nullptr;                   // a mono program's Voice::legato (Patch.h, "Mono")
        std::vector<Circuit*> circuits;            // the circuit dials' shims, in order
        bool boundsCheck = false;                  // compiled with the bounds checks (the frozen copy never is)
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
        // Each end within kInitBudget; after one that runs past it (or fails a bounds check), no other; and none at all
        // once a call has stopped the Patch, whose state may be any (Patch.h, "The loop budget").
        bool ran = stopped() == Method::none;
        endsCalled_ = 0;
        const auto end = [&](auto* fn, void* self) {
            if (!ran || fn == nullptr)
                return;
            s_->program->setLoopBudget(kInitBudget);
            ++endsCalled_;
            fn(self);
            ran = !s_->program->loopBudgetExceeded() && !s_->program->boundsViolation();   // a failed check ends them
        };
        for (std::size_t i = 0; i < instances_.size(); ++i)
        {
            if (initialised_[i])
                end(s_->end, instances_[i]);
            initialised_[i] = false;
        }
        if (sharedInitialised_)
            end(s_->sharedEnd, shared_);
        sharedInitialised_ = false;
    }

    void Patch::freeInstances() noexcept
    {
        endInstances();
        for (void* p : instances_)
            alignedFree(p);
        instances_.clear();
        initialised_.clear();
        alignedFree(shared_);
        shared_ = nullptr;
        alignedFree(host_);
        host_ = nullptr;
    }

    Patch::Result Patch::compile(std::string_view source, float sampleRate, std::string_view fileName)
    {
        return compile(singleFile(std::string(source), std::string(fileName)), sampleRate);
    }

    Patch::Result Patch::compile(const Sources& files, float sampleRate, bool keepIr, const CircuitPins* pins, bool loopBudget,
                                 bool boundsCheck)
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
        if (isCircuitFile(file))
        {
            r.errors = netlistFirst(file);
            r.compileMs = msSince(t0);
            return r;
        }
        // An #include finds the file of that name (a netlist: the module generated from it); the program's own
        // included again reads nothing, as FMinus reads each file once. The files read, in the order first read: the
        // panel's lines come from them.
        std::vector<const SourceFile*> compiled{ &files.front() };
        circuits::Generator circuits(files, pins);
        const fminus::Program::IncludeResolver include = includeOf(files, circuits, &compiled);

        // Pass 1: the program as written.
        fminus::Program::Options read;
        read.optimisation = 0;
        read.knobVersions = 0;
        read.fileName = file;
        read.defines = defines();
        read.include = include;
        std::unique_ptr<fminus::Program> first = fminus::Program::compile(source, r.errors, read);
        r.circuitJobs = circuits.jobs();
        r.circuitKeys = circuits.keys();
        if (!circuits.errors().empty())
        {
            // A netlist's errors stop the compile, ahead of FMinus's own (which they cause: its module is empty).
            r.errors = circuits.errors();
            r.compileMs = msSince(t0);
            return r;
        }
        r.errors = circuits.recite(r.errors);
        if (!first)
        {
            r.compileMs = msSince(t0);
            return r;
        }
        // FMinus's warnings, of the program as written (pass 2 appends lines that switch on nothing): notes, after the
        // netlists' (below).
        const std::string warnings = warningsOf(*first, circuits);
        const fminus::Program::Module* main = first->module(kModule);
        if (main == nullptr)
        {
            std::string have;
            for (const fminus::Program::Module& m : first->modules())
                have += (have.empty() ? "" : ", ") + m.name;
            r.errors = file + ": error: the plugin plays a module " + kModule + " (README.md, \"Writing a program\")" +
                       (have.empty() ? std::string(", and the program has no module") : ", and the program has " + have) +
                       "\n";
            r.notes += warnings;
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
        // An instrument's Shared hooks (Patch.h, "Shared on its own"): each method FMinus compiled that Lang reads as
        // declared the way the plugin calls it; one of the name declared otherwise is the program's own, never called.
        std::array<bool, lang::kSharedHooks> hooks{};
        if constexpr (kInstrument)
        {
            const auto sharedHas = [&](const char* method) {
                return sharedDecl != nullptr && std::find(sharedDecl->methods.begin(), sharedDecl->methods.end(),
                                                          std::string(kSharedModule) + "::" + method) != sharedDecl->methods.end();
            };
            bool named = false;
            for (const Call& c : kSharedHookCalls)
                named = named || sharedHas(c.method);
            if (named)
            {
                Sources texts;
                for (const SourceFile& f : files)
                    if (!isGuiFile(f.name))
                        texts.push_back(f);
                lang::Service service;
                lang::Options o;
                o.instrument = kInstrument;
                service.setOptions(o);
                service.setFiles(texts);
                const std::array<lang::SharedHook, lang::kSharedHooks> found = lang::sharedHooks(service);
                for (std::size_t k = 0; k < found.size(); ++k)
                {
                    const Call& c = kSharedHookCalls[k];
                    if (!sharedHas(c.method))
                        continue;
                    if (found[k].declared)
                    {
                        shims.push_back(&c);
                        hooks[k] = true;
                        continue;
                    }
                    const lang::SharedHook& h = found[k];
                    const std::string at = h.file >= 0 && h.file < static_cast<int>(service.files().size())
                                             ? service.files()[static_cast<std::size_t>(h.file)].name + ":"
                                                   + std::to_string(h.at.line + 1) + ":" + std::to_string(h.at.col + 1)
                                             : file;
                    r.notes += at + ": note: " + kSharedModule + "::" + c.method + " is not declared as the plugin calls it (`"
                             + c.declaration + "`), so the plugin never calls it\n";
                }
            }
        }
        if (!r.errors.empty())
        {
            r.notes += warnings;
            r.compileMs = msSince(t0);
            return r;
        }

        // The dials: the first kMaxControls parameters (the host's, then the panel's own: Controls.h), a float a stream.
        auto patch = std::unique_ptr<Patch>(new Patch());
        std::array<std::string, kMaxControls> names{};
        std::array<bool, kMaxControls> streams{};
        std::size_t dials = 0;
        for (std::size_t k = 0; k < main->parameters.size(); ++k)
        {
            const fminus::Program::Parameter& p = main->parameters[k];
            if (k >= static_cast<std::size_t>(kMaxControls))
            {
                r.notes += file + ": note: " + p.module + "::" + p.name + " has no dial (the plugin has " +
                           std::to_string(kMaxControls) + ": the host's " + std::to_string(kHostControls) + " and the panel's " +
                           std::to_string(kPanelControls) + "): it stays at its default\n";
                continue;
            }
            patch->controls_[k] = controlOf(p);
            names[k] = p.module + "::" + p.name;
            streams[k] = p.type == "float";
            dials = k + 1;
        }
        // The circuits' dials (above, "Circuits"), after the params: no FMinus parameter (no name, no slot in an
        // instance), a shim's arguments each.
        const std::vector<lang::CircuitBinding> bindings = checkedBindings(files, *main, circuits, file, static_cast<int>(dials),
                                                                           r.notes);
        std::vector<const lang::CircuitBinding*> bound;
        std::size_t mostArgs = 0;
        for (const lang::CircuitBinding& b : bindings)
        {
            if (!b.dialled())
                continue;
            bound.push_back(&b);
            CircuitCall call;
            const int pots = static_cast<int>(b.pots.size());
            for (int a = 0; a < b.args(); ++a)
            {
                const int slot = b.slots[static_cast<std::size_t>(a)];
                const std::string& name = b.names[static_cast<std::size_t>(a)];
                call.slots.push_back(slot);
                if (a < pots)
                {
                    const lang::CircuitPot& pot = b.pots[static_cast<std::size_t>(a)];
                    call.fixed.push_back(pot.def);
                    if (slot >= 0)
                        patch->controls_[static_cast<std::size_t>(slot)] = lang::potControl(name, pot.def);
                }
                else
                {
                    const lang::CircuitSwitch& w = b.switches[static_cast<std::size_t>(a - pots)];
                    call.fixed.push_back(static_cast<float>(w.def));
                    if (slot >= 0)
                    {
                        patch->controls_[static_cast<std::size_t>(slot)] = lang::switchControl(name, w.positions, w.def);
                        patch->circuitRamps_ = true;
                    }
                }
                if (slot >= 0)
                    patch->circuitMask_ |= slotBit(slot);
            }
            mostArgs = std::max(mostArgs, call.slots.size());
            patch->circuitCalls_.push_back(std::move(call));
        }
        patch->circuitArgs_.assign(mostArgs, 0.0f);
        // Mono (Patch.h): the first #pragma mono line of the files FMinus read, as it read them, and the dial it names;
        // then Voice::legato's shim, for a mono program whose Voice declares it as the plugin calls it.
        for (const SourceFile& f : asRead(compiled, files))
        {
            if (isCircuitFile(f.name))
                continue;                              // a netlist's lines are SPICE's
            std::string_view text = f.text;
            std::vector<std::string> words;
            for (int no = 1; !text.empty(); ++no)
            {
                const std::size_t end = text.find('\n');
                const std::string_view line = text.substr(0, end);
                text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
                if (!monoLine(line, words))
                    continue;
                const std::string at = f.name + ":" + std::to_string(no) + ":1: note: #pragma mono: ";
                if (!kInstrument)
                {
                    r.notes += at + "the effect plays no notes, so it is left out\n";
                    continue;
                }
                if (patch->mono_)
                {
                    r.notes += at + "the program's first such line counts, and this one is left out\n";
                    continue;
                }
                patch->mono_ = true;
                if (words.size() > 1)
                    r.notes += at + "it names one dial at most, so what follows " + words.front() + " is left out\n";
                if (words.empty())
                    continue;
                int dial = -1;
                for (int c = 0; c < kMaxControls && dial < 0; ++c)
                    if (patch->controls_[static_cast<std::size_t>(c)].used && words.front() == patch->controls_[static_cast<std::size_t>(c)].name)
                        dial = c;
                if (dial < 0)
                    r.notes += at + "no dial is named " + words.front() + ", so the program is mono always\n";
                else if (patch->controls_[static_cast<std::size_t>(dial)].type == ControlType::real)
                    r.notes += at + words.front() + " is a real dial, and its switch is a bool (or an int, on when it is not "
                                                    "0), so the program is mono always\n";
                else
                    patch->monoDial_ = dial;
            }
        }
        bool legatoShim = false;
        if constexpr (kInstrument)
        {
            if (has("legato"))
            {
                Sources texts;
                for (const SourceFile& f : files)
                    if (!isGuiFile(f.name))
                        texts.push_back(f);
                lang::Service service;
                lang::Options o;
                o.instrument = kInstrument;
                service.setOptions(o);
                service.setFiles(texts);
                const lang::SharedHook h = lang::legatoHook(service);
                const std::string at = h.file >= 0 && h.file < static_cast<int>(service.files().size())
                                         ? service.files()[static_cast<std::size_t>(h.file)].name + ":" + std::to_string(h.at.line + 1)
                                               + ":" + std::to_string(h.at.col + 1)
                                         : file;
                if (!patch->mono_)
                    r.notes += at + ": note: " + kModule + "::legato is called only in a mono program (`#pragma mono`), so the "
                                                         "plugin never calls it\n";
                else if (!h.declared)
                    r.notes += at + ": note: " + kModule + "::legato is not declared as the plugin calls it (`"
                             + kLegatoCall.declaration + "`), so the plugin never calls it, and a new note starts a voice\n";
                else
                {
                    shims.push_back(&kLegatoCall);
                    legatoShim = true;
                }
            }
        }
        fminus::Program::Options build = playOptions(file, names, streams, loopBudget, boundsCheck);
        build.include = include;
        build.keepIr = keepIr ? fminus::Program::KeepIr::Both : fminus::Program::KeepIr::None;

        // Pass 2: the program and the plugin's calls, one line each after its last.
        std::string full(source);
        if (!full.empty() && full.back() != '\n')
            full += '\n';
        const int firstShim = lineCount(full);
        for (const Call* c : shims)
            full += std::string(c->shim) + "\n";
        std::vector<std::string> circuitLines;
        for (std::size_t k = 0; k < bound.size(); ++k)
        {
            full += circuitShim(k, *bound[k]) + "\n";
            circuitLines.push_back(circuitWords(*bound[k]));
        }
        std::string errors;
        patch->s_->program = fminus::Program::compile(full, errors, build);
        if (!patch->s_->program)
        {
            r.errors = translate(circuits.recite(errors), file, firstShim, shims, circuitLines);
            r.compileMs = msSince(t0);
            return r;
        }
        fminus::Program& program = *patch->s_->program;
        Compiled& s = *patch->s_;
        s.boundsCheck = boundsCheck;
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
        if (hooks[0])
            lookShared(kSharedHookShims[0], s.sharedTick);
        if (hooks[1])
            lookShared(kSharedHookShims[1], s.sharedNoteOn);
        if (hooks[2])
            lookShared(kSharedHookShims[2], s.sharedNoteOff);
        if (legatoShim)
            lookShared("playground_legato", s.legato);
        for (std::size_t k = 0; k < bound.size(); ++k)
        {
            std::string missing;
            s.circuits.push_back(program.function<Compiled::Circuit>("playground_circuit_" + std::to_string(k), missing));
            r.errors += missing;
        }
        if (!r.errors.empty() || s.module == nullptr)
        {
            r.compileMs = msSince(t0);
            return r;
        }
        patch->params_.assign(static_cast<std::size_t>(kMaxControls), nullptr);
        for (std::size_t k = 0; k < dials; ++k)
        {
            if (names[k].empty())
                continue;                              // a circuit's dial: no parameter
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
            rc.circuits = circuits.generated();
            rc.pins = circuits.pins();
            rc.source = full;
            rc.names = names;
            rc.streams = streams;
            rc.controls = patch->controls_;
            rc.size = s.module->size;
            rc.alignment = s.module->alignment;
            rc.start = s.start != nullptr;
            rc.stop = s.stop != nullptr;
            rc.render = s.render != nullptr;
            rc.process = s.process != nullptr;
            rc.sharedTick = s.sharedTick != nullptr;
            rc.sharedNoteOn = s.sharedNoteOn != nullptr;
            rc.sharedNoteOff = s.sharedNoteOff != nullptr;
            rc.legato = s.legato != nullptr;
            rc.bindings = bindings;
            rc.circuitShims = static_cast<int>(bound.size());
            rc.loopBudget = loopBudget;
            rc.boundsCheck = boundsCheck;
        }

        // The instances: aligned, zeroed, their parameters at their defaults, init run.
        patch->instanceAlign_ = std::max<std::size_t>(s.module->alignment, alignof(std::max_align_t));
        patch->instanceSize_ = std::max<std::size_t>(s.module->size, 1);
        patch->instanceSize_ = (patch->instanceSize_ + patch->instanceAlign_ - 1) / patch->instanceAlign_ *
                               patch->instanceAlign_;
        const int count = kInstrument ? kInstances : 1;
        for (int i = 0; i < count; ++i)
        {
            void* p = alignedAlloc(patch->instanceAlign_, patch->instanceSize_);
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
            patch->shared_ = alignedAlloc(patch->sharedAlign_, patch->sharedSize_);
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
            // An instrument's: each param of Shared named as a dial reads it (Patch.h, "Shared on its own").
            if constexpr (kInstrument)
                for (const fminus::Program::Parameter& p : m->parameters)
                {
                    int dial = -1;
                    for (int c = 0; c < kMaxControls && dial < 0; ++c)
                        if (patch->controls_[static_cast<std::size_t>(c)].used && p.name == patch->controls_[static_cast<std::size_t>(c)].name)
                            dial = c;
                    if (dial < 0)
                    {
                        r.notes += file + ": note: " + kSharedModule + "::" + p.name + " reads no dial (none is named " + p.name
                                 + "), so it stays at its default\n";
                        continue;
                    }
                    Slot& slot = patch->sharedSlots_[static_cast<std::size_t>(dial)];
                    slot.stream = p.streamOffset;
                    slot.value = p.valueOffset;
                    slot.kind = p.type == "int" ? Slot::Kind::integer : p.type == "bool" ? Slot::Kind::boolean : Slot::Kind::real;
                    patch->sharedDialMask_ |= 1u << dial;
                }
        }
        // The module Host's one instance (host.fm), the fields that point at it (the main module's and Shared's), and
        // where its fields are.
        if (const fminus::Program::Module* m = program.module(kHostModule))
        {
            patch->hostAlign_ = std::max<std::size_t>(m->alignment, alignof(std::max_align_t));
            patch->hostSize_ = (std::max<std::size_t>(m->size, 1) + patch->hostAlign_ - 1) / patch->hostAlign_ * patch->hostAlign_;
            patch->host_ = alignedAlloc(patch->hostAlign_, patch->hostSize_);
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
            at("steering", "float", l.steering);
            at("throttle", "float", l.throttle);
            at("brake", "float", l.brake);
            at("clutch", "float", l.clutch);
            at("gear", "int", l.gear);
            at("buttons", "int", l.buttons);
            for (const fminus::Program::Module::Field& f : m->fields)
                if (f.name == "playing" || f.name == "sustain")
                    l.boolSize = f.size;
            if (patch->hostFields_.empty() && patch->sharedHostFields_.empty())
                r.notes += file + ": note: the program has a module Host, but no field points at it (`Host* host;` in "
                         + kModule + "), so the plugin's tempo, position and wheels reach nothing\n";
            patch->setHost(HostBlock{});
        }
        patch->sources_ = files;
        // The panel, and a tap (where it sits in an instance, or in the one Shared) for each display of a field: the
        // main module's, or Shared's through its Shared* field (displayFields: "shared->level").
        std::vector<PanelField> fields, sharedFields;
        for (const fminus::Program::Module::Field& f : s.module->fields)
            fields.push_back({ f.name, f.type });
        const fminus::Program::Module* sharedModule = patch->shared_ != nullptr ? program.module(kSharedModule) : nullptr;
        if (sharedModule != nullptr)
            for (const fminus::Program::Module::Field& f : sharedModule->fields)
                sharedFields.push_back({ f.name, f.type });
        fields = displayFields(fields, sharedFields);
        // What reading the panel needs (Patch.h, "The panel"), kept in the recipe so that an edit of the panel's files
        // is read again with no compile: the fields, each one's tap (the main module's, or Shared's through its Shared*
        // field: "shared->level"), the files FMinus read.
        PanelBase& reading = patch->recipe_.panel;
        reading.fields = fields;
        for (const PanelField& pf : fields)
        {
            if (pf.type != "float" && pf.type != "int" && pf.type != "bool")
                continue;                              // a display takes these alone (GuiSpec's, PanelSpec's)
            const std::size_t arrow = pf.name.find("->");
            const fminus::Program::Module* home = arrow == std::string::npos ? s.module : sharedModule;
            const std::string name = arrow == std::string::npos ? pf.name : pf.name.substr(arrow + 2);
            if (home != nullptr)
                for (const fminus::Program::Module::Field& f : home->fields)
                    if (f.name == name)
                    {
                        Tap t;
                        t.offset = f.offset;
                        t.type = pf.type == "float" ? Tap::Type::real : (pf.type == "int" ? Tap::Type::integer : Tap::Type::boolean);
                        t.shared = home != s.module;
                        reading.taps.emplace_back(pf.name, t);
                        break;
                    }
        }
        // The racing wheel's shift lights (lightsTap): the main module's shiftLights, else Shared's (its tap is named
        // through the Shared* field: "shared->shiftLights"). A field of the name that no tap reads is a note.
        {
            const std::string lights = kLightsField, viaShared = std::string("->") + kLightsField;
            const auto lightsName = [&](const std::string& n, bool shared) {
                return shared ? n.size() > viaShared.size() && n.compare(n.size() - viaShared.size(), viaShared.size(), viaShared) == 0
                              : n == lights;
            };
            for (const bool shared : { false, true })
            {
                for (const auto& [name, t] : reading.taps)
                    if (lightsName(name, shared))
                    {
                        patch->lights_ = t;
                        patch->hasLights_ = true;
                        break;
                    }
                if (patch->hasLights_)
                    break;
            }
            if (!patch->hasLights_)
                for (const PanelField& pf : fields)
                    if (lightsName(pf.name, false) || lightsName(pf.name, true))
                    {
                        r.notes += file + ": note: " + pf.name + " is "
                                 + (pf.type.rfind("param", 0) == 0 ? std::string("a dial") : "a " + pf.type)
                                 + ": a racing wheel's shift lights follow a float, int or bool field the program writes\n";
                        break;
                    }
        }
        for (const SourceFile* f : compiled)
            reading.read.push_back(f->name);
        // A .gui file (GuiSpec.h), the first one, makes the interface; else the #pragma panel lines do, as FMinus
        // read the files (a skipped group's lines are none).
        const std::vector<SourceFile> readTexts = asRead(compiled, files);
        std::vector<const SourceFile*> asFmRead;
        for (const SourceFile& f : readTexts)
            asFmRead.push_back(&f);
        const auto gui = std::find_if(files.begin(), files.end(), [](const SourceFile& f) { return isGuiFile(f.name); });
        if (gui != files.end())
        {
            std::string ignored;
            const PanelSpec pragmas = readPanelSpec(asFmRead, patch->controls_, fields, kInstrument, ignored);
            reading.pragmas = !pragmas.sections.empty() || !pragmas.displays.empty();
            if (reading.pragmas)
                r.notes += file + ": note: the interface is " + gui->name + "'s: the #pragma panel lines are left out\n";
            const Controls declared = patch->controls_;
            patch->panel_ = readPanel(reading, declared, files, patch->controls_, patch->taps_, r.notes);
        }
        else
        {
            patch->panel_ = readPanelSpec(asFmRead, patch->controls_, fields, kInstrument, r.notes);
            reading.pragmas = !patch->panel_.sections.empty() || !patch->panel_.displays.empty();
            for (const PanelDisplay& d : patch->panel_.displays)
                if (d.tap >= 0)
                    for (const auto& [name, t] : reading.taps)
                        if (name == d.source)
                        {
                            patch->taps_.push_back(t);
                            break;
                        }
        }
        // The probe table (Patch.h, "Probes"): each main-module field of a netlist's module (a scalar or an array), its
        // elements and the probe fields' offsets in one, each checked against its type and the sizes; the netlists'
        // modules, whose probe fields the field map leaves out.
        for (const SourceFile& g : circuits.generated())
            if (const circuits::Generator::Shape* shape = circuits.shape(g.name); shape != nullptr && !shape->module.empty())
                patch->circuitModules_.push_back(shape->module);
        for (const fminus::Program::Module::Field& f : s.module->fields)
        {
            std::string base;
            std::size_t extent = 0;
            if (f.type.rfind("param ", 0) == 0 || !heldModule(f.type, base, extent) || extent == 0 || f.size % extent != 0)
                continue;
            std::string netlist;
            const circuits::Generator::Shape* shape = circuits.shapeOfModule(base, &netlist);
            const fminus::Program::Module* m = shape != nullptr ? program.module(base) : nullptr;
            if (m == nullptr || f.offset + f.size > s.module->size || f.size / extent < m->size)
                continue;
            ProbeEntry e;
            e.base = f.offset;
            e.stride = f.size / extent;
            e.elements = static_cast<int>(extent);
            bool ok = true;
            const auto at = [&](const char* name, const char* type, std::size_t size, std::size_t& out) {
                const auto x = std::find_if(m->fields.begin(), m->fields.end(),
                                            [&](const fminus::Program::Module::Field& y) { return y.name == name; });
                if (x == m->fields.end() || squeezed(x->type) != type || x->size != size || x->offset + x->size > m->size)
                    ok = false;
                else
                    out = x->offset;
            };
            const std::string slots = "[" + std::to_string(circuits::kProbeSlots) + "]";
            at("prb_", "int", sizeof(int32_t), e.prb);
            at("prs_", ("int" + slots).c_str(), sizeof(int32_t) * circuits::kProbeSlots, e.prs);
            at("prn_", "int", sizeof(int32_t), e.prn);
            at("pr_", ("float[" + std::to_string(circuits::kProbeSlots * circuits::kProbeRing) + "]").c_str(),
               sizeof(float) * circuits::kProbeSlots * circuits::kProbeRing, e.pr);
            at("prp_", ("float" + slots).c_str(), sizeof(float) * circuits::kProbeSlots, e.prp);
            at("pra_", ("float" + slots).c_str(), sizeof(float) * circuits::kProbeSlots, e.pra);
            at("prq_", ("float" + slots).c_str(), sizeof(float) * circuits::kProbeSlots, e.prq);
            if (!ok)
                continue;
            for (const circuits::Node& n : shape->nodes)
                e.readable.push_back(n.readable ? 1 : 0);
            circuits::Probed d;
            d.field = f.name;
            d.module = base;
            d.file = netlist;
            d.nodes = shape->nodes;
            std::vector<std::size_t> dims;
            for (std::size_t open = f.type.find('['); open != std::string::npos; open = f.type.find('[', open + 1))
                dims.push_back(static_cast<std::size_t>(std::max(1L, std::strtol(f.type.c_str() + open + 1, nullptr, 10))));
            for (std::size_t k = 0; k < extent; ++k)
            {
                std::string index;
                std::size_t rest = k;
                for (std::size_t dd = dims.size(); dd-- > 0;)
                {
                    index = "[" + std::to_string(rest % dims[dd]) + "]" + index;
                    rest /= dims[dd];
                }
                d.paths.push_back(f.name + index);
            }
            patch->probes_.push_back(std::move(e));
            patch->recipe_.probes.push_back(std::move(d));
        }
        patch->mapFields();
        patch->reset(sampleRate);
        if (const Method stop = patch->stopped(); stop != Method::none)
        {
            // An init (or Shared's) that ran past its budget, or failed a bounds check: the program does not play
            // (Patch.h, "The loop budget", "Bounds checks").
            r.errors = stopMessage(files, stop, patch->rate_, &patch->bounds_);
            r.stopped = stop;
            r.bounds = patch->bounds_;
            r.notes = circuits.warnings() + circuits.notes() + warnings + r.notes;
            r.compileMs = msSince(t0);
            return r;
        }
        if (keepIr)
        {
            r.ir.lowered = program.loweredIr();
            r.ir.optimised = program.optimisedIr();
        }
        // The netlists' warnings and provisional notes first, then FMinus's warnings: what they say is about the files
        // the program reads.
        r.notes = circuits.warnings() + circuits.notes() + warnings + r.notes;
        r.compileMs = msSince(t0);
        r.patch = std::move(patch);
        return r;
    }

    PanelSpec Patch::readPanel(const PanelBase& base, const Controls& declared, const Sources& files, Controls& controls,
                               std::vector<Tap>& taps, std::string& notes)
    {
        controls = declared;
        taps.clear();
        const SourceFile* gui = nullptr;
        for (const SourceFile& f : files)
            if (isGuiFile(f.name))
            {
                if (gui == nullptr)
                    gui = &f;
                else
                    notes += f.name + ": note: the interface is " + gui->name + "'s (the first .gui file); this one is "
                                                                               "left out\n";
            }
        if (gui == nullptr)
            return {};
        std::vector<std::string> fileNames;
        for (const SourceFile& f : files)
            fileNames.push_back(f.name);
        PanelSpec panel = readGuiSpec(*gui, controls, base.fields, kInstrument, fileNames, notes);
        for (const PanelDisplay& d : panel.displays)
            if (d.tap >= 0)
                for (const auto& [name, t] : base.taps)
                    if (name == d.source)
                    {
                        taps.push_back(t);
                        break;
                    }
        return panel;
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
        perSample_ = budgetOf(1, rate_) / kBudgetFloor;
        endInstances();
        // FMinus's record of a failed bounds check emptied first: while it holds one, every call returns partway,
        // init's too (Patch.h, "Bounds checks").
        s_->program->clearBoundsViolation();
        bounds_ = Bounds{};
        stop_.store(static_cast<uint8_t>(Method::none), std::memory_order_relaxed);
        initsCalled_ = 0;
        // Each init within kInitBudget (Patch.h, "The loop budget"): one that runs past it, or fails a bounds check,
        // stops the Patch there.
        const auto init = [&](Compiled::Init* fn, void* self, Method call) {
            if (fn == nullptr)
                return true;
            s_->program->setLoopBudget(kInitBudget);
            ++initsCalled_;
            fn(self, rate_);
            return within(*s_, call);
        };
        // Shared first: the instances' init may read what its init made.
        if (shared_ != nullptr)
        {
            std::memset(shared_, 0, sharedSize_);
            setDefaults(shared_, kSharedModule, 0);
            for (int c = 0; c < kMaxControls; ++c)   // a param named as a dial: at the dial's default, as the instances'
                if (((sharedDialMask_ >> c) & 1u) != 0)
                    store(shared_, sharedSlots_[static_cast<std::size_t>(c)], controls_[static_cast<std::size_t>(c)].def);
            for (const std::size_t at : sharedHostFields_)
                std::memcpy(static_cast<char*>(shared_) + at, &host_, sizeof host_);
            if (!init(s_->sharedInit, shared_, Method::sharedInit))
                return;                              // no end for it, and the instances not initialised
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
            if (!init(s_->init, p, Method::init))
                return;                              // no end for it, nor for the instances after it
            initialised_[i] = true;
        }
    }

    // ---- the loop budget ----------------------------------------------------------------------------------------------
    std::int64_t Patch::budgetOf(int samples, float rate) noexcept
    {
        const auto perSample = static_cast<std::int64_t>(kLoopsPerSecond / static_cast<double>(rate > 0.0f ? rate : 48000.0f));
        return perSample * std::max(samples, kBudgetFloor);
    }

    std::int64_t Patch::budgetLeft() const noexcept
    {
        return code().program->loopBudgetLeft();
    }

    void Patch::budget(const Compiled& c, int samples) noexcept
    {
        lastBudget_ = perSample_ * std::max(samples, kBudgetFloor);
        c.program->setLoopBudget(lastBudget_);
    }

    bool Patch::within(const Compiled& c, Method call) noexcept
    {
        if (!outOfBounds(c) && !c.program->loopBudgetExceeded())
            return true;
        stop_.store(static_cast<uint8_t>(call), std::memory_order_release);   // after bounds_
        return false;
    }

    // ---- the bounds checks --------------------------------------------------------------------------------------------
    namespace
    {
        template <std::size_t N>
        void copyText(char (&to)[N], std::string_view from) noexcept
        {
            const std::size_t n = std::min(from.size(), N - 1);
            std::memcpy(to, from.data(), n);
            to[n] = '\0';
        }
    }

    bool Patch::outOfBounds(const Compiled& c) noexcept
    {
        if (!c.boundsCheck)
            return false;
        const std::optional<fminus::Program::BoundsViolation> v = c.program->boundsViolation();   // no allocation
        if (!v)
            return false;
        Bounds b;
        b.failed = true;
        copyText(b.file, v->file);
        copyText(b.array, v->array);
        copyText(b.type, v->type);
        b.line = v->line;
        b.column = v->column;
        b.width = v->width;
        b.extent = v->extent;
        b.index = v->index;
        bounds_ = b;
        return true;
    }

    std::string Patch::Bounds::where() const
    {
        return std::string(file) + ":" + std::to_string(line) + ":" + std::to_string(column);
    }

    std::string Patch::Bounds::what() const
    {
        // FMinus's own words (BoundsViolation::message), without the place it starts with.
        fminus::Program::BoundsViolation v;
        v.file = file;
        v.line = line;
        v.column = column;
        v.array = array;
        v.type = type;
        v.extent = extent;
        v.index = index;
        v.width = width;
        std::string words = v.message();
        const std::string at = where() + ": ";
        if (words.compare(0, at.size(), at) == 0)
            words.erase(0, at.size());
        return words;
    }

    std::string Patch::callName(Method call)
    {
        switch (call)
        {
            case Method::init:       return "init()";
            case Method::end:        return "end()";
            case Method::process:    return "process()";
            case Method::start:      return "start()";
            case Method::stop:       return "stop()";
            case Method::circuit:    return "a circuit's set()";
            case Method::sharedInit: return std::string(kSharedModule) + "'s init()";
            case Method::sharedEnd:  return std::string(kSharedModule) + "'s end()";
            case Method::sharedTick: return std::string(kSharedModule) + "'s tick()";
            case Method::sharedNoteOn: return std::string(kSharedModule) + "'s noteOn()";
            case Method::sharedNoteOff: return std::string(kSharedModule) + "'s noteOff()";
            case Method::legato:     return "legato()";
            case Method::none:       break;
        }
        return "a call";
    }

    std::string Patch::stopWhy(Method call, const Bounds* bounds)
    {
        if (bounds != nullptr && bounds->failed)
            return callName(call) + " went outside an array (" + bounds->where() + ": " + bounds->what() + ")";
        return callName(call) + " ran past its budget (a loop that never ends?)";
    }

    std::string Patch::stopMessage(const Sources& files, Method call, float rate, const Bounds* bounds)
    {
        if (bounds != nullptr && bounds->failed)
        {
            // At the access, as FMinus places it (an #include's file by its name): what failed, then what the checks
            // are.
            const bool offline = call == Method::init || call == Method::end || call == Method::sharedInit
                              || call == Method::sharedEnd;
            const std::string at = bounds->where();
            return at + ": error: " + callName(call) + " went outside an array: " + bounds->what() + "\n" + at
                 + ": note: the plugin checks each index into an array the program declares while it plays the program as "
                   "written (not the frozen copy that plays once the dials hold still), stopped the call before the access, "
                 + (offline ? "and the program does not play\n" : "and plays the program no more\n");
        }
        // The method's line, where the program defines it (Lang's index): the call the plugin made, which a loop in a
        // function it calls stops too.
        const bool shared = call == Method::sharedInit || call == Method::sharedEnd || call == Method::sharedTick
                         || call == Method::sharedNoteOn || call == Method::sharedNoteOff;
        const char* method = call == Method::init || call == Method::sharedInit ? "init"
                           : call == Method::end || call == Method::sharedEnd   ? "end"
                           : call == Method::process                          ? "process"
                           : call == Method::start                            ? "start"
                           : call == Method::stop                             ? "stop"
                           : call == Method::legato                           ? "legato"
                           : call == Method::sharedTick                       ? "tick"
                           : call == Method::sharedNoteOn                     ? "noteOn"
                           : call == Method::sharedNoteOff                    ? "noteOff"
                                                                            : nullptr;
        std::string at = files.empty() ? std::string(kProgramName) : files.front().name;
        if (method != nullptr && !files.empty())
        {
            Sources read;
            for (const SourceFile& f : files)
                if (!isGuiFile(f.name) && !isCircuitFile(f.name))
                    read.push_back(f);
            lang::Service service;
            lang::Options o;
            o.instrument = kInstrument;
            service.setOptions(o);
            service.setFiles(read);
            const std::string module = shared ? kSharedModule : kModule;
            for (const lang::Symbol& s : service.symbols())
                if (s.kind == lang::Kind::method && s.body && s.name == method && s.container == module && s.file >= 0
                    && s.file < static_cast<int>(service.files().size()))
                {
                    at = service.files()[static_cast<std::size_t>(s.file)].name + ":" + std::to_string(s.at.line + 1) + ":"
                       + std::to_string(s.at.col + 1);
                    break;
                }
        }
        const bool offline = call == Method::init || call == Method::end || call == Method::sharedInit || call == Method::sharedEnd;
        const auto grouped = [](std::int64_t v) {
            std::string digits = std::to_string(v), out;
            for (std::size_t k = 0; k < digits.size(); ++k)
            {
                if (k > 0 && (digits.size() - k) % 3 == 0)
                    out += ',';
                out += digits[k];
            }
            return out;
        };
        const std::string note = offline
            ? "the plugin stops an init or end past " + grouped(kInitBudget) + " loop iterations (a loop in a function it "
              "calls counts too), so the program does not play"
            : "the plugin stops a call past " + grouped(budgetOf(1, rate) / kBudgetFloor) + " loop iterations a sample at "
              + std::to_string(std::lround(rate)) + " Hz, about what a core runs in real time when an iteration takes a "
              "cycle (a loop in a function it calls counts too), and plays the program no more";
        return at + ": error: " + callName(call) + " ran past its budget: a loop that never ends?\n" + at + ": note: "
             + note + "\n";
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
        put(l.steering, h.steering);
        put(l.throttle, h.throttle);
        put(l.brake, h.brake);
        put(l.clutch, h.clutch);
        put(l.gear, static_cast<int32_t>(h.gear));
        put(l.buttons, static_cast<int32_t>(h.buttons));
    }

    // The audio thread's calls, each within its budget (Patch.h, "The loop budget"), none once the Patch has stopped.
    void Patch::process(float* left, float* right, int n) noexcept
    {
        const Compiled& c = code();
        if (c.process == nullptr || instances_.empty() || stopped() != Method::none)
            return;
        budget(c, n);
        c.process(instances_[0], left, right, n);
        within(c, Method::process);
    }

    void Patch::start(int i, float note, float velocity) noexcept
    {
        const Compiled& c = code();
        if (c.start == nullptr || i < 0 || i >= static_cast<int>(instances_.size()) || stopped() != Method::none)
            return;
        budget(c, 0);
        c.start(instances_[static_cast<std::size_t>(i)], note, velocity);
        within(c, Method::start);
    }

    void Patch::legato(int i, float note, float velocity) noexcept
    {
        const Compiled& c = code();
        if (c.legato == nullptr || i < 0 || i >= static_cast<int>(instances_.size()) || stopped() != Method::none)
            return;
        budget(c, 0);
        c.legato(instances_[static_cast<std::size_t>(i)], note, velocity);
        within(c, Method::legato);
    }

    void Patch::stop(int i) noexcept
    {
        const Compiled& c = code();
        if (c.stop == nullptr || i < 0 || i >= static_cast<int>(instances_.size()) || stopped() != Method::none)
            return;
        budget(c, 0);
        c.stop(instances_[static_cast<std::size_t>(i)]);
        within(c, Method::stop);
    }

    void Patch::setCircuits(const float* plain, int i) noexcept
    {
        const Compiled& c = code();
        if (i < 0 || i >= static_cast<int>(instances_.size()) || c.circuits.size() != circuitCalls_.size()
            || stopped() != Method::none)
            return;
        void* self = instances_[static_cast<std::size_t>(i)];
        for (std::size_t k = 0; k < circuitCalls_.size(); ++k)
        {
            const CircuitCall& call = circuitCalls_[k];
            for (std::size_t a = 0; a < call.slots.size(); ++a)
                circuitArgs_[a] = call.slots[a] >= 0 ? plain[call.slots[a]] : call.fixed[a];
            if (c.circuits[k] == nullptr)
                continue;
            budget(c, 0);
            c.circuits[k](self, circuitArgs_.data());
            if (!within(c, Method::circuit))
                return;
        }
    }

    bool Patch::render(int i, float* left, float* right, int n) noexcept
    {
        const Compiled& c = code();
        if (c.render == nullptr || i < 0 || i >= static_cast<int>(instances_.size()) || stopped() != Method::none)
            return false;
        budget(c, n);
        const bool sounding = c.render(instances_[static_cast<std::size_t>(i)], left, right, n);
        return within(c, Method::process) && sounding;
    }

    // ---- Shared on its own (Patch.h) ------------------------------------------------------------------------------------
    void Patch::setSharedDials(const float* plain) noexcept
    {
        if (shared_ == nullptr)
            return;
        for (uint32_t m = sharedDialMask_; m != 0; m &= m - 1)
        {
            const int c = std::countr_zero(m);
            store(shared_, sharedSlots_[static_cast<std::size_t>(c)], plain[c]);
        }
    }

    void Patch::sharedTick(int n) noexcept
    {
        const Compiled& c = code();
        if (c.sharedTick == nullptr || shared_ == nullptr || stopped() != Method::none)
            return;
        budget(c, n);
        c.sharedTick(shared_, n);
        within(c, Method::sharedTick);
    }

    void Patch::sharedNoteOn(float note, float velocity) noexcept
    {
        const Compiled& c = code();
        if (c.sharedNoteOn == nullptr || shared_ == nullptr || stopped() != Method::none)
            return;
        budget(c, 0);
        c.sharedNoteOn(shared_, note, velocity);
        within(c, Method::sharedNoteOn);
    }

    void Patch::sharedNoteOff(float note) noexcept
    {
        const Compiled& c = code();
        if (c.sharedNoteOff == nullptr || shared_ == nullptr || stopped() != Method::none)
            return;
        budget(c, 0);
        c.sharedNoteOff(shared_, note);
        within(c, Method::sharedNoteOff);
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
                                                 std::string& why, bool exactMath, std::string* ir)
    {
        const auto t0 = std::chrono::steady_clock::now();
        if (r.files.empty() || r.source.empty())
        {
            why = "no program";
            return nullptr;
        }
        fminus::Program::Options build = frozenOptions(r, plain, exactMath);
        circuits::Generator circuits(r.files, r.circuits);    // the modules the program was compiled with
        build.include = includeOf(r.files, circuits);
        build.keepIr = ir != nullptr ? fminus::Program::KeepIr::Optimised : fminus::Program::KeepIr::None;
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
        for (int k = 0; k < r.circuitShims; ++k)                  // the circuit dials' shims, which the source carries
            s.circuits.push_back(program.function<Compiled::Circuit>("playground_circuit_" + std::to_string(k), missing));
        // Shared's hooks (Patch.h, "Shared on its own"), which run on the audio thread through the copy's code too.
        if (r.sharedTick)
            s.sharedTick = program.function<Compiled::Tick>(kSharedHookShims[0], missing);
        if (r.sharedNoteOn)
            s.sharedNoteOn = program.function<Compiled::Start>(kSharedHookShims[1], missing);
        if (r.sharedNoteOff)
            s.sharedNoteOff = program.function<Compiled::NoteOff>(kSharedHookShims[2], missing);
        if (r.legato)                                             // a mono program's (Patch.h, "Mono")
            s.legato = program.function<Compiled::Start>("playground_legato", missing);
        if (!missing.empty())
        {
            why = missing;
            return nullptr;
        }
        f->program_ = r.program;
        f->values_ = plain;
        if (ir != nullptr)
            *ir = program.optimisedIr();
        f->ms_ = msSince(t0);
        return f;
    }

    // ---- the IR alone ---------------------------------------------------------------------------------------------------
    bool Patch::ir(const Recipe& r, Ir& out, std::string& why)
    {
        if (r.files.empty() || r.source.empty())
        {
            why = "no program";
            return false;
        }
        fminus::Program::Options o = playOptions(r.files.front().name, r.names, r.streams, r.loopBudget, r.boundsCheck);
        circuits::Generator circuits(r.files, r.circuits);
        o.include = includeOf(r.files, circuits);
        o.keepIr = fminus::Program::KeepIr::Both;
        std::string errors;
        const std::unique_ptr<fminus::Program> program = fminus::Program::compile(r.source, errors, o);
        if (!program)
        {
            why = errors.empty() ? std::string("it did not compile") : errors;
            return false;
        }
        out.lowered = program->loweredIr();
        out.optimised = program->optimisedIr();
        return true;
    }

    bool Patch::frozenIr(const Recipe& r, const std::array<float, kMaxControls>& plain, std::string& out, std::string& why)
    {
        if (r.files.empty() || r.source.empty())
        {
            why = "no program";
            return false;
        }
        fminus::Program::Options o = frozenOptions(r, plain, false);
        circuits::Generator circuits(r.files, r.circuits);
        o.include = includeOf(r.files, circuits);
        o.keepIr = fminus::Program::KeepIr::Optimised;
        std::string errors;
        const std::unique_ptr<fminus::Program> program = fminus::Program::compile(r.source, errors, o);
        if (!program)
        {
            why = errors.empty() ? std::string("it did not compile") : errors;
            return false;
        }
        out = program->optimisedIr();
        return true;
    }

    float Patch::tap(int t, int i) const noexcept
    {
        if (t < 0 || t >= static_cast<int>(taps_.size()))
            return 0.0f;
        return tapOf(taps_[static_cast<std::size_t>(t)], i);
    }

    float Patch::tapOf(const Tap& k, int i) const noexcept
    {
        const void* home = k.shared ? shared_ : i >= 0 && i < static_cast<int>(instances_.size()) ? instances_[static_cast<std::size_t>(i)] : nullptr;
        if (home == nullptr)
            return 0.0f;
        const char* at = static_cast<const char*>(home) + k.offset;
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
            const std::vector<std::string>& circuitModules;   // their probe fields are the probes', never mapped
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
                const bool circuit = std::find(circuitModules.begin(), circuitModules.end(), m.name) != circuitModules.end();
                for (const fminus::Program::Module::Field& f : m.fields)
                {
                    const std::string path = prefix + f.name;
                    if (f.type.rfind("param ", 0) == 0)
                        continue;                      // a dial (or a held module's parameter at its default)
                    if (circuit && circuits::isProbeField(f.name))
                        continue;                      // the probes' (Patch.h, "Probes"), not the circuit's
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
        Mapper map{ program, fields_, unmapped_, circuitModules_, false };
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

    // ---- the probes (Patch.h, "Probes") ----------------------------------------------------------------------------------
    int Patch::probeElements(int circuit) const noexcept
    {
        return circuit >= 0 && circuit < static_cast<int>(probes_.size()) ? probes_[static_cast<std::size_t>(circuit)].elements : 0;
    }

    char* Patch::probeElement(int c, int e, int i) const noexcept
    {
        if (c < 0 || c >= static_cast<int>(probes_.size()) || i < 0 || i >= static_cast<int>(instances_.size()))
            return nullptr;
        const ProbeEntry& p = probes_[static_cast<std::size_t>(c)];
        if (e < 0 || e >= p.elements)
            return nullptr;
        return static_cast<char*>(instances_[static_cast<std::size_t>(i)]) + p.base + static_cast<std::size_t>(e) * p.stride;
    }

    uint8_t Patch::armProbes(int c, int e, int i, const int16_t (&nodes)[circuits::kProbeSlots]) noexcept
    {
        char* at = probeElement(c, e, i);
        if (at == nullptr)
            return 0;
        const ProbeEntry& p = probes_[static_cast<std::size_t>(c)];
        uint8_t armed = 0;
        for (int k = 0; k < circuits::kProbeSlots; ++k)
        {
            const int n = nodes[k];
            const bool readable = n >= 0 && n < static_cast<int>(p.readable.size()) && p.readable[static_cast<std::size_t>(n)] != 0;
            const int32_t v = readable ? n : -1;
            std::memcpy(at + p.prs + sizeof(int32_t) * static_cast<std::size_t>(k), &v, sizeof v);
            armed = static_cast<uint8_t>(armed | (readable ? 1u << k : 0u));
        }
        const int32_t on = armed != 0 ? 1 : 0;
        std::memcpy(at + p.prb, &on, sizeof on);
        probeWrites_.fetch_add(1, std::memory_order_relaxed);
        return armed;
    }

    void Patch::disarmProbes(int c, int e, int i) noexcept
    {
        char* at = probeElement(c, e, i);
        if (at == nullptr)
            return;
        const int32_t off = 0;
        std::memcpy(at + probes_[static_cast<std::size_t>(c)].prb, &off, sizeof off);
        probeWrites_.fetch_add(1, std::memory_order_relaxed);
    }

    bool Patch::probeArmed(int c, int e, int i) const noexcept
    {
        const char* at = probeElement(c, e, i);
        if (at == nullptr)
            return false;
        int32_t on = 0;
        std::memcpy(&on, at + probes_[static_cast<std::size_t>(c)].prb, sizeof on);
        return on != 0;
    }

    bool Patch::probeCount(int c, int e, int i, uint32_t& prn) const noexcept
    {
        const char* at = probeElement(c, e, i);
        if (at == nullptr)
            return false;
        int32_t n = 0;
        std::memcpy(&n, at + probes_[static_cast<std::size_t>(c)].prn, sizeof n);
        prn = static_cast<uint32_t>(n);
        return true;
    }

    const float* Patch::probeRing(int c, int e, int i, int slot) const noexcept
    {
        const char* at = probeElement(c, e, i);
        if (at == nullptr || slot < 0 || slot >= circuits::kProbeSlots)
            return nullptr;
        return reinterpret_cast<const float*>(at + probes_[static_cast<std::size_t>(c)].pr
                                              + sizeof(float) * static_cast<std::size_t>(circuits::kProbeRing * slot));
    }

    bool Patch::takeProbeStats(int c, int e, int i, float (&peak)[circuits::kProbeSlots], float (&sum)[circuits::kProbeSlots],
                               float (&squares)[circuits::kProbeSlots]) noexcept
    {
        char* at = probeElement(c, e, i);
        if (at == nullptr)
            return false;
        const ProbeEntry& p = probes_[static_cast<std::size_t>(c)];
        std::memcpy(peak, at + p.prp, sizeof peak);
        std::memcpy(sum, at + p.pra, sizeof sum);
        std::memcpy(squares, at + p.prq, sizeof squares);
        std::memset(at + p.prp, 0, sizeof peak);
        std::memset(at + p.pra, 0, sizeof sum);
        std::memset(at + p.prq, 0, sizeof squares);
        probeWrites_.fetch_add(1, std::memory_order_relaxed);
        return true;
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
    // main module and its required methods. Nothing appended, no second pass, no instance. Every netlist of the files is
    // generated, the ones the program does not include too, so that a tab's mistakes show while it is typed; its
    // warnings join the messages. No job: the self-test waits for a RUN.
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
        circuits::Generator circuits(files);
        if (isCircuitFile(file))
        {
            r.errors = netlistFirst(file);
            circuits.generateRest();
            r.errors += circuits.errors() + circuits.warnings();
            r.compileMs = msSince(t0);
            return r;
        }
        fminus::Program::Options read;
        read.optimisation = 0;
        read.knobVersions = 0;
        read.fileName = file;
        read.defines = defines();
        read.include = includeOf(files, circuits);
        const std::unique_ptr<fminus::Program> first = fminus::Program::compile(files.front().text, r.errors, read);
        const bool netlistErrors = !circuits.errors().empty();
        circuits.generateRest();
        if (netlistErrors)
        {
            // As compile(): a netlist's errors stop it, ahead of FMinus's own; every netlist's messages.
            r.errors = circuits.errors() + circuits.warnings();
            r.compileMs = msSince(t0);
            return r;
        }
        r.errors = circuits.recite(r.errors) + circuits.errors() + circuits.warnings();
        if (first)
            r.errors += warningsOf(*first, circuits);            // FMinus's warnings, as RUN's notes have them
        if (first)
        {
            const fminus::Program::Module* main = first->module(kModule);
            if (main == nullptr)
                r.errors += file + ": error: the plugin plays a module " + kModule + " (README.md, \"Writing a program\")\n";
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
