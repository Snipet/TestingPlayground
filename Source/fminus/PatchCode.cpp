// Source/fminus/PatchCode.cpp: where a program's machine code is (Patch.h, "where the code is"), for measure_output's
// profile (dsp/Profile.h): FMinus's code ranges (Program::codeRanges, its recipe 37), named as the program names its
// functions, the plugin's appended calls (Patch.cpp's playground_ lines) as the methods they call.
#include "fminus/Patch.h"

#include "Product.h"

#include "fminus/FMinus.h"

#include <string_view>

namespace playground
{
    namespace
    {
        constexpr const char* kModule = product::kIsInstrument ? "Voice" : "Effect";
        constexpr std::string_view kCall = "playground_";
        constexpr std::string_view kSharedCall = "playground_shared_";
        constexpr std::string_view kCircuitCall = "playground_circuit_";
    }

    std::vector<Patch::CodeRange> Patch::codeRanges(std::string& errors) const
    {
        std::vector<CodeRange> out;
        const std::vector<fminus::Program::CodeRange> ranges = fminusProgram().codeRanges(errors);
        out.reserve(ranges.size());
        for (const fminus::Program::CodeRange& r : ranges)
        {
            CodeRange c;
            c.address = r.address;
            c.size = r.size;
            c.module = r.module;
            c.parallel = r.kind == fminus::Program::CodeRange::Kind::Parallel;
            const std::string_view f = r.function;
            if (r.kind == fminus::Program::CodeRange::Kind::Support || f.empty())
                c.function = "FMinus's math";
            else if (f.substr(0, kCircuitCall.size()) == kCircuitCall)
                c.function = "the circuit dials' set()";
            else if (f.substr(0, kSharedCall.size()) == kSharedCall)
            {
                c.module = "Shared";
                c.function = "Shared::" + std::string(f.substr(kSharedCall.size()));
            }
            else if (f.substr(0, kCall.size()) == kCall)
            {
                c.module = kModule;
                c.function = std::string(kModule) + "::" + std::string(f.substr(kCall.size()));
            }
            else
                c.function = r.function;
            for (const std::string& name : r.inlined)
                if (name != c.function)
                    c.inlined.push_back(name);
            out.push_back(std::move(c));
        }
        return out;
    }
}
