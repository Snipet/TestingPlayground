// Tools/probes/common/ProbeMain.cpp: main() of the probe executable (modelled on FSynth's ProbeMain.cpp, simplified).
//
//   <exe> <layer>.<name> [--golden-root <dir>] [--arch arm64] [--results <dir>] [--only <glob>] [--verbose]
//   <exe> --list | --help
//
// It finds the probe, fills in the harness flags a person running it by hand would rather not type (--golden-root: a
// directory that need not exist, since the probes have no golden rows; --arch: this machine's), prepares the probe's
// preferences sandbox (<ENV_PREFIX>PREFS_DIR, emptied when it lies below a `sandbox` directory: CTest's
// <build>/sandbox/<test>), and runs the body under flush-to-zero. An exception escaping the body is a harness error
// (exit 4). The exit code is always the harness's finish().
#include "Probe.h"

#include "Product.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace probe
{
    namespace
    {
        constinit const Registration* gHead = nullptr;
    }

    Registration::Registration(const char* layerDotName, Fn f) noexcept : name(layerDotName), fn(f), next(gHead)
    {
        gHead = this;
    }

    const Registration* registrations() noexcept { return gHead; }

    std::filesystem::path outDir()
    {
        std::filesystem::path dir = PROBE_DEFAULT_OUT_DIR;              // <build>/probe-out (cmake/Probes.cmake)
        if (const char* env = std::getenv("PROBE_OUT_DIR"); env != nullptr && *env != '\0')
            dir = env;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    std::string outPath(const std::string& name)
    {
        return (outDir() / name).string();
    }
}

namespace
{
    constexpr int kExitHarnessError = 4;

    std::vector<const probe::Registration*> sorted()
    {
        std::vector<const probe::Registration*> v;
        for (const probe::Registration* r = probe::registrations(); r != nullptr; r = r->next)
            v.push_back(r);
        std::sort(v.begin(), v.end(), [](const auto* a, const auto* b) { return std::strcmp(a->name, b->name) < 0; });
        return v;
    }

    bool hasFlag(int argc, char** argv, std::string_view flag)
    {
        for (int i = 1; i < argc; ++i)
            if (argv[i] != nullptr && flag == argv[i])
                return true;
        return false;
    }

    // The probe's preferences directory (FunkGui's <ENV_PREFIX>PREFS_DIR), emptied first when it is a CTest sandbox.
    std::string makeSandbox()
    {
        namespace fs = std::filesystem;
        const std::string var = std::string(testingplaygroundsynth::product::kEnvPrefix) + "PREFS_DIR";
        const char* dir = std::getenv(var.c_str());
        if (dir == nullptr || *dir == '\0')
            return {};
        const fs::path p = fs::path(dir).lexically_normal();
        bool belowSandbox = false;
        bool seen = false;
        for (const fs::path& part : p)
        {
            if (seen && !part.empty())
                belowSandbox = true;
            if (part == "sandbox")
                seen = true;
        }
        std::error_code ec;
        if (p.is_absolute() && belowSandbox)
            fs::remove_all(p, ec);
        fs::create_directories(p, ec);
        return ec ? "cannot create " + var + " " + p.string() + ": " + ec.message() : std::string();
    }
}

int main(int argc, char** argv)
{
    const std::vector<const probe::Registration*> probes = sorted();
    for (std::size_t i = 1; i < probes.size(); ++i)
        if (std::strcmp(probes[i - 1]->name, probes[i]->name) == 0)
        {
            std::fprintf(stderr, "HARNESS ERROR  probe %s is registered twice\n", probes[i]->name);
            return kExitHarnessError;
        }

    const std::string_view command = argc > 1 && argv[1] != nullptr ? argv[1] : "";
    if (command.empty() || command == "--help" || command == "-h" || command == "--list")
    {
        if (command != "--list")
            std::printf("usage: %s <layer>.<name> [--results <dir>] [--only <glob>] [--verbose]\n       %s --list\n"
                        "probes:\n", argv[0], argv[0]);
        for (const probe::Registration* r : probes)
            std::printf("%s%s\n", command == "--list" ? "" : "  ", r->name);
        return command.empty() ? kExitHarnessError : 0;
    }

    const auto it = std::find_if(probes.begin(), probes.end(), [&](const auto* r) { return command == r->name; });
    if (it == probes.end())
    {
        std::fprintf(stderr, "HARNESS ERROR  unknown probe '%.*s' (--list shows them)\n",
                     static_cast<int>(command.size()), command.data());
        return kExitHarnessError;
    }

    // The harness requires --golden-root and --arch; fill them in for a run by hand.
    std::vector<std::string> extra;
    if (!hasFlag(argc, argv, "--golden-root"))
    {
        extra.emplace_back("--golden-root");
        extra.push_back((probe::outDir() / "no-goldens").string());
    }
    if (!hasFlag(argc, argv, "--arch"))
    {
        extra.emplace_back("--arch");
        extra.emplace_back("arm64");
    }
    std::vector<char*> args(argv, argv + argc);
    for (std::string& s : extra)
        args.push_back(s.data());
    args.push_back(nullptr);

    funkgui::test::Probe P((*it)->name, "", static_cast<int>(args.size()) - 1, args.data());
    if (const std::string err = makeSandbox(); !err.empty())
        P.harnessError(err);
    {
        const funkgui::test::ScopedFtz ftz;
        try
        {
            (*it)->fn(P);
        }
        catch (const std::exception& e)
        {
            P.harnessError(std::string("uncaught exception: ") + e.what());
        }
        catch (...)
        {
            P.harnessError("uncaught exception of unknown type");
        }
    }
    return P.finish();
}
