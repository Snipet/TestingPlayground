// Source/fminus/PatchCall.cpp: one of a program's functions called by name, for the chat's run_function (Patch.h, "one
// function called by name"), through FMinus's call by signature (Program::callable and call: the arguments checked
// against the declaration, the result or what went wrong as a value). Off the audio thread, on a Patch of the caller's
// own; and the state that plays, copied by the audio thread (Engine::Snapshot), taken into such a Patch (adopt) when
// none of its pointers would reach memory of the program that plays.
#include "fminus/Patch.h"

#include "Product.h"

#include "fminus/FMinus.h"

#include "fsdsp/core/ScopedFtz.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#if defined(_WIN32)
#include <malloc.h>                                  // _aligned_malloc
#endif

namespace playground
{
    namespace
    {
        using Program = fminus::Program;
        using Value = Program::Value;
        using Kind = Value::Kind;

        constexpr const char* kModule = product::kIsInstrument ? "Voice" : "Effect";
        constexpr const char* kSharedModule = "Shared";
        constexpr const char* kHostModule = "Host";

        // A module held by value: its name and each dimension's extent ("Osc" {}, "Osc[8]" {8}, "Osc[2][3]" {2, 3});
        // a pointer is none.
        bool heldModule(const std::string& type, std::string& base, std::vector<std::size_t>& dims)
        {
            dims.clear();
            if (type.find('*') != std::string::npos || type.rfind("param ", 0) == 0)
                return false;
            const std::size_t open = type.find('[');
            base = type.substr(0, open);
            while (!base.empty() && base.back() == ' ')
                base.pop_back();
            for (std::size_t at = open; at != std::string::npos; at = type.find('[', at + 1))
                dims.push_back(static_cast<std::size_t>(std::max(1L, std::strtol(type.c_str() + at + 1, nullptr, 10))));
            return !base.empty();
        }

        std::size_t elementsOf(const std::vector<std::size_t>& dims)
        {
            std::size_t n = 1;
            for (const std::size_t d : dims)
                n *= d;
            return n;
        }

        // Every field of module `m` at `at`, and of the modules it holds by value (each element of an array of them),
        // with its path: fn(field, its module, path, its address). At most 16 deep.
        using FieldFn = std::function<void(const Program::Module::Field&, const Program::Module&, const std::string&, const char*)>;
        void walk(const Program& p, const Program::Module& m, const char* at, const std::string& path, int depth, const FieldFn& fn)
        {
            if (depth > 16)
                return;
            for (const Program::Module::Field& f : m.fields)
            {
                const std::string here = path + f.name;
                fn(f, m, here, at + f.offset);
                std::string base;
                std::vector<std::size_t> dims;
                const Program::Module* held = heldModule(f.type, base, dims) ? p.module(base) : nullptr;
                const std::size_t count = elementsOf(dims);
                if (held == nullptr || count == 0 || f.size % count != 0)
                    continue;
                const std::size_t each = f.size / count;
                for (std::size_t k = 0; k < count; ++k)
                {
                    std::string index;
                    std::size_t rest = k;
                    for (std::size_t d = dims.size(); d-- > 0;)
                    {
                        index = "[" + std::to_string(rest % dims[d]) + "]" + index;
                        rest /= dims[d];
                    }
                    walk(p, *held, at + f.offset + k * each, here + index + ".", depth + 1, fn);
                }
            }
        }

        const char* kindName(Kind k)
        {
            switch (k)
            {
                case Kind::Bool: return "bool";
                case Kind::Int: return "int";
                case Kind::Float: return "float";
                case Kind::Double: return "double";
                case Kind::Pointer: return "pointer";
                case Kind::Void: return "void";
            }
            return "void";
        }

        // A number as the result shows it: a float's 9 significant digits, a double's 17 (each reads back as it was),
        // an int whole, a bool's word.
        std::string numberText(double v, Kind k)
        {
            char b[48];
            if (k == Kind::Bool)
                return v != 0.0 ? "true" : "false";
            if (k == Kind::Int)
                std::snprintf(b, sizeof b, "%lld", static_cast<long long>(v));
            else if (k == Kind::Double)
                std::snprintf(b, sizeof b, "%.17g", v);
            else
                std::snprintf(b, sizeof b, "%.9g", v);
            return b;
        }

        std::string fieldText(double v, Patch::Field::Type t)
        {
            switch (t)
            {
                case Patch::Field::Type::boolean: return numberText(v, Kind::Bool);
                case Patch::Field::Type::integer: return numberText(v, Kind::Int);
                case Patch::Field::Type::real64: return numberText(v, Kind::Double);
                case Patch::Field::Type::real: return numberText(v, Kind::Float);
            }
            return numberText(v, Kind::Float);
        }

        // "const float *x", "float drive": a parameter as C declares it.
        std::string declared(const std::string& type, const std::string& name)
        {
            if (name.empty())
                return type;
            return !type.empty() && type.back() == '*' ? type + name : type + " " + name;
        }

        // An instance of the caller's own (a module's, or an array's elements), aligned, zeroed, freed with it.
        struct Block
        {
            void* p = nullptr;
            Block() = default;
            Block(std::size_t align, std::size_t bytes)
            {
                const std::size_t a = std::max(align, alignof(std::max_align_t));
                const std::size_t n = (std::max<std::size_t>(bytes, 1) + a - 1) / a * a;
#if defined(_WIN32)
                p = _aligned_malloc(n, a);
#else
                p = std::aligned_alloc(a, n);
#endif
                if (p != nullptr)
                    std::memset(p, 0, n);
            }
            ~Block()
            {
#if defined(_WIN32)
                _aligned_free(p);
#else
                std::free(p);
#endif
            }
            Block(const Block&) = delete;
            Block& operator=(const Block&) = delete;
        };

        // One buffer of a pointer argument: its elements as the parameter's type, then the guard's zeros.
        struct Buffer
        {
            std::size_t param = 0;                     // its parameter's index (a method's `this` counted)
            Kind element = Kind::Float;
            std::size_t shown = 0, size = 0;           // the elements given (or extent), and with the guard
            std::unique_ptr<Block> block;
            double at(std::size_t i) const
            {
                if (element == Kind::Int)
                    return static_cast<double>(static_cast<const int32_t*>(block->p)[i]);
                if (element == Kind::Double)
                    return static_cast<const double*>(block->p)[i];
                return static_cast<double>(static_cast<const float*>(block->p)[i]);
            }
        };

        std::size_t elementBytes(Kind k) { return k == Kind::Double ? sizeof(double) : sizeof(int32_t); }
    }

    // ---- the layout ---------------------------------------------------------------------------------------------------
    uint64_t Patch::hashLayout(const fminus::Program& program)
    {
        uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](const void* data, std::size_t n) {
            const auto* b = static_cast<const unsigned char*>(data);
            for (std::size_t i = 0; i < n; ++i)
                h = (h ^ b[i]) * 1099511628211ull;
        };
        const auto text = [&](const std::string& s) { mix(s.data(), s.size() + 1); };   // its NUL too: "ab" "c" != "a" "bc"
        const auto size = [&](std::size_t n) {
            const uint64_t v = n;
            mix(&v, sizeof v);
        };
        for (const fminus::Program::Module& m : program.modules())
        {
            text(m.name);
            size(m.size);
            size(m.alignment);
            for (const fminus::Program::Module::Field& f : m.fields)
            {
                text(f.name);
                text(f.type);
                size(f.offset);
                size(f.size);
            }
        }
        return h;
    }

    Patch::Layout Patch::layout() const noexcept
    {
        Layout l;
        l.instance = instances_.empty() ? 0 : instanceSize_;
        l.instanceAlign = instanceAlign_;
        l.shared = shared_ != nullptr ? sharedSize_ : 0;
        l.sharedAlign = sharedAlign_;
        l.host = host_ != nullptr ? hostSize_ : 0;
        l.hostAlign = hostAlign_;
        l.hash = layoutHash_;
        return l;
    }

    std::string Patch::adopt(const void* instance, const void* shared, const void* host)
    {
        const fminus::Program& p = fminusProgram();
        const fminus::Program::Module* main = p.module(kModule);
        if (main == nullptr || instances_.empty())
            return "the program has no " + std::string(kModule);
        const fminus::Program::Module* sharedModule = shared_ != nullptr ? p.module(kSharedModule) : nullptr;
        if ((shared_ != nullptr) != (shared != nullptr) || (host_ != nullptr) != (host != nullptr))
            return "the copy is not of this program";
        // Every pointer the copy holds must be null, but those the plugin points (the main module's Shared* and Host*,
        // Shared's Host*, a sample's data): any other points at memory of the program that plays.
        std::string sharing;
        // A sample's data (#pragma sample, Patch.h's "Samples") is the plugin's too, a sound the Patch holds: this one
        // points the copy's at its own below, as its compile with the same samples made it.
        std::vector<std::size_t> mainSamples, sharedSamples;
        for (const SampleUse& u : samples_)
            for (const SamplePlace& place : u.places)
                (place.shared ? sharedSamples : mainSamples).push_back(place.data);
        const auto check = [&](const std::vector<std::size_t>& repointed, const std::vector<std::size_t>& sampled, const char* base,
                               const std::string& prefix) {
            return [&, base, prefix](const fminus::Program::Module::Field& f, const fminus::Program::Module&, const std::string& path,
                                     const char* at) {
                if (!sharing.empty() || f.type.find('*') == std::string::npos)
                    return;
                const std::size_t offset = static_cast<std::size_t>(at - base);
                if (path.find('.') == std::string::npos
                    && std::find(repointed.begin(), repointed.end(), offset) != repointed.end())
                    return;
                if (std::find(sampled.begin(), sampled.end(), offset) != sampled.end())
                    return;
                for (std::size_t k = 0; k < f.size; ++k)
                    if (at[k] != 0)
                    {
                        sharing = prefix + path + " (" + f.type + ")";
                        return;
                    }
            };
        };
        std::vector<std::size_t> mainPointed = sharedFields_;
        mainPointed.insert(mainPointed.end(), hostFields_.begin(), hostFields_.end());
        const char* in = static_cast<const char*>(instance);
        if (in != nullptr)
            walk(p, *main, in, "", 0, check(mainPointed, mainSamples, in, ""));
        if (sharing.empty() && sharedModule != nullptr)
        {
            const char* sh = static_cast<const char*>(shared);
            walk(p, *sharedModule, sh, "", 0, check(sharedHostFields_, sharedSamples, sh, "Shared's "));
        }
        if (!sharing.empty())
            return "its field " + sharing + " points at memory of the program that plays (a block its init allocated, or "
                   "Shared's), which a call on a copy would share";

        if (instance != nullptr)
            std::memcpy(instances_[0], instance, instanceSize_);
        if (shared_ != nullptr)
            std::memcpy(shared_, shared, sharedSize_);
        if (host_ != nullptr)
            std::memcpy(host_, host, hostSize_);
        for (const std::size_t at : sharedFields_)
            std::memcpy(static_cast<char*>(instances_[0]) + at, &shared_, sizeof shared_);
        for (const std::size_t at : hostFields_)
            std::memcpy(static_cast<char*>(instances_[0]) + at, &host_, sizeof host_);
        if (shared_ != nullptr)
            for (const std::size_t at : sharedHostFields_)
                std::memcpy(static_cast<char*>(shared_) + at, &host_, sizeof host_);
        fillSamples(instances_[0], false);             // the samples' sounds this Patch holds (its data, frames, rate)
        if (shared_ != nullptr)
            fillSamples(shared_, true);
        // Every parameter's stream at none (a dial that glided as it was copied pointed at the Engine's samples): each
        // reads its value, as a call that holds still does.
        const auto unstream = [](const fminus::Program::Module::Field& f, const fminus::Program::Module& m, const std::string&,
                                 const char* at) {
            if (f.type.rfind("param ", 0) != 0)
                return;
            for (const fminus::Program::Parameter& q : m.parameters)
                if (q.name == f.name)
                {
                    char* slot = const_cast<char*>(at) - f.offset + q.streamOffset;
                    std::memset(slot, 0, sizeof(void*));
                }
        };
        walk(p, *main, static_cast<const char*>(instances_[0]), "", 0, unstream);
        if (sharedModule != nullptr)
            walk(p, *sharedModule, static_cast<const char*>(shared_), "", 0, unstream);
        return {};
    }

    // ---- the call -----------------------------------------------------------------------------------------------------
    Patch::CallOutcome Patch::call(const std::string& name, const std::vector<CallArg>& args,
                                   const std::function<std::string(bool instance)>& onState)
    {
        CallOutcome out;
        bool onInstance = false;                       // it runs on instance 0, or a field it holds (onState's)
        fminus::Program& p = fminusProgram();
        if (name.rfind("playground_", 0) == 0)
        {
            out.error = "'" + name + "' is one of the plugin's own calls into the program, not the program's";
            return out;
        }

        // What it runs on: a method's instance (above), found by the name.
        void* self = nullptr;
        std::unique_ptr<Block> own;
        std::string callName = name;
        const std::size_t dot = name.rfind('.');
        const std::size_t colons = name.find("::");
        if (dot != std::string::npos || name.find("->") != std::string::npos)
        {
            // A field's method by its path: "filter.tick", "osc[2].env.next"; through the main module's Shared* field
            // ("shared->table.read", or by that field's own name), and Shared's own method ("shared->build").
            const fminus::Program::Module* m = p.module(kModule);
            char* at = instances_.empty() ? nullptr : static_cast<char*>(instances_[0]);
            std::string where = kModule;
            std::string rest = name;
            if (const std::size_t arrow = name.find("->"); arrow != std::string::npos)
            {
                const std::string via = name.substr(0, arrow);
                bool pointer = via == "shared";
                if (m != nullptr)
                    for (const fminus::Program::Module::Field& f : m->fields)
                        pointer = pointer || (f.name == via && f.type.find('*') != std::string::npos
                                              && f.type.rfind(kSharedModule, 0) == 0 && f.type.size() > 6
                                              && (f.type[6] == ' ' || f.type[6] == '*'));
                if (!pointer)
                {
                    out.error = "'" + via + "' is no pointer to Shared in " + std::string(kModule) + " (shared->x.method)";
                    return out;
                }
                m = shared_ != nullptr ? p.module(kSharedModule) : nullptr;
                at = static_cast<char*>(shared_);
                where = kSharedModule;
                rest = name.substr(arrow + 2);
                if (m != nullptr && rest.find('.') == std::string::npos)
                {
                    callName = std::string(kSharedModule) + "::" + rest;   // Shared's own method, on its one
                    self = shared_;
                    out.where = CallOutcome::On::shared;
                    out.on = kSharedModule;
                }
            }
            if (m == nullptr || at == nullptr)
            {
                out.error = std::string("the program has no ") + (where == kSharedModule ? "module Shared" : "main module");
                return out;
            }
            const std::size_t lastDot = rest.rfind('.');
            if (self == nullptr && (lastDot == std::string::npos || lastDot + 1 >= rest.size()))
            {
                out.error = "'" + name + "' names no method: a field's method is its path, a dot, then the method (filter.tick)";
                return out;
            }
            std::string path = self == nullptr ? rest.substr(0, lastDot) : std::string();
            const std::string method = self == nullptr ? rest.substr(lastDot + 1) : std::string();
            std::size_t from = 0;
            while (self == nullptr && from <= path.size())
            {
                const std::size_t end = std::min(path.find('.', from), path.size());
                const std::string segment = path.substr(from, end - from);
                const std::size_t open = segment.find('[');
                const std::string field = segment.substr(0, open);
                std::vector<std::size_t> index;
                bool good = !field.empty();
                for (std::size_t b = open; good && b != std::string::npos; b = segment.find('[', b + 1))
                {
                    char* stop = nullptr;
                    const long v = std::strtol(segment.c_str() + b + 1, &stop, 10);
                    good = stop != nullptr && *stop == ']' && v >= 0 && stop != segment.c_str() + b + 1;
                    index.push_back(static_cast<std::size_t>(std::max(0L, v)));
                }
                if (!good)
                {
                    out.error = "'" + segment + "' in '" + name + "' is no field's name (a field, or an array's element: osc[2])";
                    return out;
                }
                const auto f = std::find_if(m->fields.begin(), m->fields.end(),
                                            [&](const fminus::Program::Module::Field& x) { return x.name == field; });
                if (f == m->fields.end())
                {
                    std::string held;
                    for (const fminus::Program::Module::Field& x : m->fields)
                    {
                        std::string base;
                        std::vector<std::size_t> dims;
                        if (heldModule(x.type, base, dims) && p.module(base) != nullptr)
                            held += (held.empty() ? "" : ", ") + x.name + " (" + x.type + ")";
                    }
                    out.error = m->name + " has no field '" + field + "'" +
                                (held.empty() ? std::string(", and holds no module") : "; the modules it holds: " + held);
                    return out;
                }
                std::string base;
                std::vector<std::size_t> dims;
                const fminus::Program::Module* next = heldModule(f->type, base, dims) ? p.module(base) : nullptr;
                if (next == nullptr)
                {
                    out.error = "'" + field + "' is a " + f->type + ", not a module the " + m->name + " holds: a method runs on a "
                                "module";
                    return out;
                }
                if (index.size() != dims.size())
                {
                    out.error = "'" + field + "' is " + f->type + ": give " + std::to_string(dims.size()) + " index"
                              + (dims.size() == 1 ? "" : "es") + " (" + field + (dims.empty() ? "" : "[0]") + (dims.size() > 1 ? "[0]" : "") + ")";
                    return out;
                }
                std::size_t flat = 0;
                for (std::size_t d = 0; d < dims.size(); ++d)
                {
                    if (index[d] >= dims[d])
                    {
                        out.error = "index " + std::to_string(index[d]) + " is past '" + field + "' (" + f->type + ")";
                        return out;
                    }
                    flat = flat * dims[d] + index[d];
                }
                const std::size_t count = std::max<std::size_t>(elementsOf(dims), 1);
                at += f->offset + flat * (f->size / count);
                m = next;
                from = end + 1;
            }
            if (self == nullptr)
            {
                self = at;
                callName = m->name + "::" + method;
                onInstance = where != kSharedModule;
                out.where = CallOutcome::On::field;
                out.on = (where == kSharedModule ? name.substr(0, name.find("->") + 2) : std::string()) + path;
            }
        }
        else if (colons != std::string::npos)
        {
            const std::string module = name.substr(0, colons);
            if (module == kModule && !instances_.empty())
            {
                self = instances_[0];
                onInstance = true;
                out.where = CallOutcome::On::main;
                out.on = kModule;
            }
            else if (module == kSharedModule && shared_ != nullptr)
            {
                self = shared_;
                out.where = CallOutcome::On::shared;
                out.on = kSharedModule;
            }
            else if (module == kHostModule && host_ != nullptr)
            {
                self = host_;
                out.where = CallOutcome::On::shared;
                out.on = kHostModule;
            }
            else if (const fminus::Program::Module* m = p.module(module))
            {
                own = std::make_unique<Block>(m->alignment, m->size);
                if (own->p == nullptr)
                {
                    out.error = "no memory for an instance of " + module;
                    return out;
                }
                setDefaults(own->p, module, 0);
                self = own->p;
                out.where = CallOutcome::On::own;
                out.on = module;
            }
        }

        std::string errors;
        const fminus::Program::Callable* c = p.callable(callName, errors);
        if (c == nullptr)
        {
            out.error = errors;
            while (!out.error.empty() && out.error.back() == '\n')
                out.error.pop_back();
            if (colons == std::string::npos && dot == std::string::npos)
            {
                // A method named as a function: say whose.
                std::string methods;
                for (const fminus::Program::Module& m : p.modules())
                    for (const std::string& x : m.methods)
                        if (x.size() > name.size() + 2 && x.compare(x.size() - name.size() - 2, std::string::npos, "::" + name) == 0)
                            methods += (methods.empty() ? "" : ", ") + x;
                if (!methods.empty())
                    out.error += " (a method of that name: " + methods + ")";
            }
            return out;
        }
        const std::size_t first = c->module != nullptr ? 1 : 0;
        {
            std::string params;
            for (std::size_t k = first; k < c->parameters.size(); ++k)
                params += (params.empty() ? "" : ", ") + declared(c->parameters[k].type, c->parameters[k].name);
            out.signature = c->resultType + " " + c->name + "(" + params + ")";
        }
        if (c->module == nullptr)
            out.where = CallOutcome::On::function;
        else if (self == nullptr)
        {
            out.error = "'" + c->name + "' is a method: it runs on an instance";
            return out;
        }

        // The arguments, each as its parameter is declared (call() converts nothing).
        const std::size_t want = c->parameters.size() - first;
        if (args.size() != want)
        {
            out.error = "'" + c->name + "' takes " + std::to_string(want) + " argument" + (want == 1 ? "" : "s") + ", not "
                      + std::to_string(args.size()) + ": " + out.signature;
            return out;
        }
        // FMinus checks no access through a pointer parameter (its extent is the caller's to know), so each array's
        // buffer covers C's idiom (buffer, n): as many elements as the largest int argument too, and as many zeros
        // again after them. A length past what run_function passes, beside an array, is refused: past its buffer, a
        // write would reach the plugin's own memory, and a read memory that may be none.
        std::size_t length = 0;
        bool arrays = false;
        for (std::size_t j = 0; j < args.size(); ++j)
        {
            const fminus::Program::Callable::Param& q = c->parameters[first + j];
            arrays = arrays || (q.kind == Kind::Pointer && q.module == nullptr && q.element != Kind::Void);
            const CallArg& a = args[j];
            if (q.kind == Kind::Int && a.kind == CallArg::Kind::number && a.number >= 0.0 && a.number <= 2147483647.0
                && a.number == std::trunc(a.number))
                length = std::max(length, static_cast<std::size_t>(a.number));   // (one that is no int: refused below)
        }
        if (arrays && length > kMaxCallElements)
        {
            out.error = "'" + c->name + "' takes an array, and an int argument of " + std::to_string(length) + " beside it is past "
                      "the " + std::to_string(kMaxCallElements) + " elements an array of run_function's holds: as a length, the "
                      "function would run past its buffer";
            return out;
        }

        std::vector<Value> values;
        std::vector<Buffer> buffers;
        buffers.reserve(args.size());
        if (c->module != nullptr)
            values.push_back(Value::instance(self, *c->module));
        for (std::size_t j = 0; j < args.size(); ++j)
        {
            const fminus::Program::Callable::Param& q = c->parameters[first + j];
            const CallArg& a = args[j];
            const std::string which = "argument " + std::to_string(j + 1) + " of '" + c->name + "'";
            const std::string its = " (its parameter " + (q.name.empty() ? std::string() : "'" + q.name + "', ") + "'" + q.type + "')";
            const std::string given = a.kind == CallArg::Kind::array     ? std::string("an array")
                                    : a.kind == CallArg::Kind::boolean ? (a.number != 0.0 ? "true" : "false")
                                                                       : numberText(a.number, Kind::Double);
            if (q.kind == Kind::Pointer)
            {
                if (q.module != nullptr)
                {
                    out.error = which + " is a module's instance" + its + ": run_function passes one only as a method's own "
                                "(call a method of " + q.module->name + ")";
                    return out;
                }
                if (q.element == Kind::Void)
                {
                    out.error = which + " is a 'void *'" + its + ": run_function passes arrays of ints, floats and doubles";
                    return out;
                }
                if (a.kind != CallArg::Kind::array)
                {
                    out.error = which + " is " + given + ", not an array" + its;
                    return out;
                }
                Buffer b;
                b.param = first + j;
                b.element = q.element;
                b.shown = std::max(a.elements.size(), a.extent);
                if (b.shown > kMaxCallElements)
                {
                    out.error = which + " has " + std::to_string(b.shown) + " elements, past the " + std::to_string(kMaxCallElements)
                              + " run_function passes";
                    return out;
                }
                const std::size_t covered = std::max(b.shown, length);
                b.size = covered + std::max(covered, kCallGuard);
                b.block = std::make_unique<Block>(alignof(double), b.size * elementBytes(q.element));
                if (b.block->p == nullptr)
                {
                    out.error = "no memory for " + which;
                    return out;
                }
                for (std::size_t i = 0; i < a.elements.size(); ++i)
                {
                    const double v = a.elements[i];
                    if (q.element == Kind::Int)
                    {
                        if (v != std::trunc(v) || v < -2147483648.0 || v > 2147483647.0)
                        {
                            out.error = "element " + std::to_string(i) + " of " + which + " is " + numberText(v, Kind::Double)
                                      + ", not an int" + its;
                            return out;
                        }
                        static_cast<int32_t*>(b.block->p)[i] = static_cast<int32_t>(v);
                    }
                    else if (q.element == Kind::Double)
                        static_cast<double*>(b.block->p)[i] = v;
                    else
                        static_cast<float*>(b.block->p)[i] = static_cast<float>(v);
                }
                if (q.element == Kind::Int)
                    values.push_back(q.readOnly ? Value::array(static_cast<const int*>(b.block->p))
                                                : Value::array(static_cast<int*>(b.block->p)));
                else if (q.element == Kind::Double)
                    values.push_back(q.readOnly ? Value::array(static_cast<const double*>(b.block->p))
                                                : Value::array(static_cast<double*>(b.block->p)));
                else
                    values.push_back(q.readOnly ? Value::array(static_cast<const float*>(b.block->p))
                                                : Value::array(static_cast<float*>(b.block->p)));
                buffers.push_back(std::move(b));
                continue;
            }
            if (a.kind == CallArg::Kind::array)
            {
                out.error = which + " is an array, not a '" + kindName(q.kind) + "'" + its;
                return out;
            }
            switch (q.kind)
            {
                case Kind::Void:
                case Kind::Pointer:
                    out.error = which + " is of a type run_function does not pass" + its;
                    return out;
                case Kind::Bool:
                    if (a.kind != CallArg::Kind::boolean)
                    {
                        out.error = which + " is " + given + ", not a bool (true or false)" + its;
                        return out;
                    }
                    values.push_back(Value::of(a.number != 0.0));
                    break;
                case Kind::Int:
                    if (a.kind != CallArg::Kind::number || a.number != std::trunc(a.number) || a.number < -2147483648.0
                        || a.number > 2147483647.0)
                    {
                        out.error = which + " is " + given + ", not an int" + its;
                        return out;
                    }
                    values.push_back(Value::of(static_cast<int>(a.number)));
                    break;
                case Kind::Float:
                case Kind::Double:
                    if (a.kind != CallArg::Kind::number)
                    {
                        out.error = which + " is " + given + ", not a '" + kindName(q.kind) + "'" + its;
                        return out;
                    }
                    values.push_back(q.kind == Kind::Float ? Value::of(static_cast<float>(a.number)) : Value::of(a.number));
                    break;
            }
        }

        // Checked: what it runs on is the state's (the main module's instance 0, Shared, Host, a field of theirs),
        // which the caller may make ready now (run_function's "playing": the copy of what plays adopted).
        if (onState && (out.where == CallOutcome::On::main || out.where == CallOutcome::On::shared
                        || out.where == CallOutcome::On::field))
            if (std::string why = onState(onInstance); !why.empty())
            {
                out.error = std::move(why);
                return out;
            }

        // The fields as they were (instance 0's, Shared's, Host's), to say what a method changed.
        std::vector<double> before(fields_.size());
        for (std::size_t k = 0; k < fields_.size(); ++k)
            before[k] = read(fields_[k], 0);

        fminus::Program::CallResult r;
        {
            const fsdsp::ScopedFtz ftz;              // the mode the programs are compiled for (Patch.h)
            p.clearBoundsViolation();
            p.setLoopBudget(kInitBudget);
            r = p.call(*c, values);
        }
        using Status = fminus::Program::CallResult::Status;
        out.ran = r.status == Status::Ok || r.status == Status::OutOfBudget || r.status == Status::OutOfBounds;
        out.stopped = r.status == Status::OutOfBudget || r.status == Status::OutOfBounds;
        if (r.status == Status::OutOfBounds && r.violation)
            out.error = "an index went outside an array (" + r.violation->message() + ")";
        else if (r.status == Status::OutOfBudget)
            out.error = "its loop budget ran out (about a second of a core's work): a loop that never ends?";
        else if (r.status != Status::Ok)
            out.error = r.message();
        if (out.stopped)
            callStopped_ = true;                     // its state may be any: no end runs (endInstances)
        if (r.status == Status::Ok && c->result != Kind::Void)
        {
            out.returned = true;
            out.value = numberText(r.value.number, c->result);
        }
        if (!out.ran)
            return out;
        for (const Buffer& b : buffers)
        {
            const fminus::Program::Callable::Param& q = c->parameters[b.param];
            CallOutcome::Array a;
            a.name = q.name;
            a.type = q.type;
            a.written = !q.readOnly;
            a.elements.reserve(b.shown);
            for (std::size_t i = 0; i < b.shown; ++i)
                a.elements.push_back(b.at(i));
            for (std::size_t i = b.shown; i < b.size && !a.past; ++i)
                a.past = b.at(i) != 0.0;
            out.arrays.push_back(std::move(a));
        }
        for (std::size_t k = 0; k < fields_.size(); ++k)
        {
            const double now = read(fields_[k], 0);
            if (std::memcmp(&now, &before[k], sizeof now) == 0)
                continue;
            if (out.changed.size() >= kMaxChanged)
            {
                ++out.moreChanged;
                continue;
            }
            out.changed.push_back(fields_[k].path + ": " + fieldText(before[k], fields_[k].type) + " -> "
                                  + fieldText(now, fields_[k].type));
        }
        return out;
    }
}
