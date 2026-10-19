// Source/fminus/FMinusProgram.cpp: FMinusProgram on FMinus's front end, code generator and JIT, as FMinus's own
// driver (tools/fminus/main.cpp) runs them: parse, lower for this machine's vector width, then hand the module to an
// LLJIT that optimises it at the chosen level and compiles a function when it is first looked up.
#include "fminus/FMinusProgram.h"

#include "frontend/Frontend.h"
#include "ir/CodeGen.h"
#include "jit/Jit.h"
#include "support/HostTarget.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Target/TargetMachine.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace testingplaygroundsynth
{
    struct FMinusProgram::Impl
    {
        std::unique_ptr<::fminus::jit::Jit> jit;
        unsigned polyLanes = 4;
        std::vector<std::string> nonBlocking;   // the functions a declaration marks [[clang::nonblocking]]
    };

    namespace
    {
        llvm::OptimizationLevel optimisationLevel(int level)
        {
            const std::array<llvm::OptimizationLevel, 4> levels{llvm::OptimizationLevel::O0,
                                                                llvm::OptimizationLevel::O1,
                                                                llvm::OptimizationLevel::O2,
                                                                llvm::OptimizationLevel::O3};
            return levels[static_cast<std::size_t>(std::clamp(level, 0, 3))];
        }
    } // namespace

    FMinusProgram::FMinusProgram(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    FMinusProgram::~FMinusProgram() = default;

    std::unique_ptr<FMinusProgram> FMinusProgram::compile(std::string_view source, std::string& errors,
                                                          const Options& options)
    {
        errors.clear();
        ::fminus::Diagnostics diagnostics(options.fileName);
        const std::optional<::fminus::ast::Program> program = ::fminus::frontend::parse(source, diagnostics);
        if (!program)
        {
            errors = diagnostics.format();
            return nullptr;
        }

        // The machine decides a poly float's width, as it does for fminus.
        const llvm::OptimizationLevel level = optimisationLevel(options.optimisation);
        auto machine = ::fminus::hostTargetMachine(::fminus::codeGenLevel(level));
        if (!machine)
        {
            errors = llvm::toString(machine.takeError());
            return nullptr;
        }
        ::fminus::ir::Options lowering;
        lowering.polyLanes = ::fminus::polyLanes(**machine);

        auto context = std::make_unique<llvm::LLVMContext>();
        std::unique_ptr<llvm::Module> module = ::fminus::ir::lower(*program, *context, diagnostics, lowering);
        if (!module)
        {
            errors = diagnostics.format();
            return nullptr;
        }
        auto jit = ::fminus::jit::Jit::create(level);
        if (!jit)
        {
            errors = llvm::toString(jit.takeError());
            return nullptr;
        }
        if (llvm::Error error = (*jit)->add(std::move(module), std::move(context)))
        {
            errors = llvm::toString(std::move(error));
            return nullptr;
        }
        auto impl = std::make_unique<Impl>();
        impl->jit = std::move(*jit);
        impl->polyLanes = lowering.polyLanes;
        for (const ::fminus::ast::Function& function : program->functions)
        {
            for (const ::fminus::ast::Attribute& attribute : function.attributes)
            {
                if (attribute.name == "clang::nonblocking")
                    impl->nonBlocking.push_back(function.name);
            }
        }
        return std::unique_ptr<FMinusProgram>(new FMinusProgram(std::move(impl)));
    }

    std::uintptr_t FMinusProgram::address(std::string_view name, std::string& errors)
    {
        auto found = impl_->jit->lookup(llvm::StringRef(name.data(), name.size()));
        if (!found)
        {
            errors = llvm::toString(found.takeError());
            return 0;
        }
        errors.clear();
        return static_cast<std::uintptr_t>(found->getValue());
    }

    unsigned FMinusProgram::polyLanes() const
    {
        return impl_->polyLanes;
    }

    bool FMinusProgram::nonBlocking(std::string_view name) const
    {
        return std::ranges::find(impl_->nonBlocking, name) != impl_->nonBlocking.end();
    }
} // namespace testingplaygroundsynth
