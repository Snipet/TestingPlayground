// Tools/probes/common/ProbeMain.cpp: main() of the probe executable (modelled on FSynth's ProbeMain.cpp, simplified).
//
//   <exe> <layer>.<name> [--golden-root <dir>] [--arch arm64] [--results <dir>] [--only <glob>] [--verbose]
//   <exe> --list | --help
//
// It finds the probe, fills in the harness flags a person running it by hand would rather not type (--golden-root: a
// directory that need not exist, since the probes have no golden rows; --arch: this machine's), prepares the probe's
// preferences sandbox (<ENV_PREFIX>PREFS_DIR, emptied when it lies below a `sandbox` directory: CTest's
// <build>/sandbox/<test>), and runs the body under flush-to-zero. The prefix is FunkGui's (funkgui_configure_product's
// ENV_PREFIX: the product's, or the installer's), so one main serves every probe executable. An exception escaping the
// body is a harness error (exit 4). The exit code is always the harness's finish().
//
// On Windows a probe that dies (heap corruption, an access violation, the C runtime's invalid-parameter check, an
// abort) would leave CTest an exit code alone: stdout is a pipe there and fully buffered (Windows' C runtime has no
// line buffering), and what it held dies with the process. So there stdout and stderr are unbuffered, and a crash
// prints what it was and the stack, raw (module and offset: no heap, which may be what broke) and then with the
// functions and lines from the probe's PDB, on stderr. The process then ends as it would have.
#include "Probe.h"

#include <funkgui/core/Config.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <atomic>
#include <csignal>
#include <cstdint>
#include <stdlib.h>
#include <windows.h>
#include <dbghelp.h>
#undef near                                          // windows.h's 16-bit leftovers
#undef far
#endif

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

    void skip(funkgui::test::Probe& P, std::string_view key, std::string_view why)
    {
        std::printf("SKIP       %.*s  %.*s\n", static_cast<int>(key.size()), key.data(), static_cast<int>(why.size()), why.data());
        std::string note = "skip_";
        for (const char c : key)
            note += (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '_';
        note.resize(std::min<std::size_t>(note.size(), 64));
        P.note(note, funkgui::test::detail::jsonString(why));
    }

    std::string emulated()
    {
#if defined(__linux__)
        std::ifstream in("/proc/cpuinfo");
        std::string line;
        while (std::getline(in, line))
        {
            if (line.find("VirtualApple") != std::string::npos)
                return "an x86-64 build run by Rosetta on Apple silicon, whose timings are not a machine's";
        }
#endif
        return {};
    }

    std::string noDisplay()
    {
#if defined(__linux__)
        if (const char* d = std::getenv("DISPLAY"); d == nullptr || *d == '\0')
            return "no X display ($DISPLAY unset): a JUCE window needs one (a desktop session, or Xvfb with a window manager)";
#endif
        return {};
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
        const std::string var = std::string(funkgui::config::kEnvPrefix) + "PREFS_DIR";
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

#if defined(_WIN32)
    // ---- Windows: what a crash leaves -------------------------------------------------------------------------------
    std::atomic<bool> gReporting{ false };

    // Straight to stderr's handle: no C runtime buffer, no heap.
    void say(const char* text) noexcept
    {
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    }

    // The return addresses from `context` (the thread's registers where it failed), or from the caller when there is
    // none, walked with the modules' unwind tables alone. A frame with no unwind data (JIT'd code) ends the walk, but
    // for the first, which may be a leaf.
    int walk(const CONTEXT* context, void** frames, int max) noexcept
    {
        if (context == nullptr)
            return static_cast<int>(CaptureStackBackTrace(2, static_cast<DWORD>(max), frames, nullptr));
        CONTEXT c = *context;
        int n = 0;
        while (n < max && c.Rip != 0)
        {
            frames[n++] = reinterpret_cast<void*>(c.Rip);
            DWORD64 imageBase = 0;
            if (PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr); f != nullptr)
            {
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, f, &c, &handlerData, &establisher, nullptr);
            }
            else if (n == 1 && c.Rsp != 0)
            {
                c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
                c.Rsp += 8;
            }
            else
                break;
        }
        return n;
    }

    void printRaw(void* const* frames, int n) noexcept
    {
        char line[600];
        for (int i = 0; i < n; ++i)
        {
            HMODULE module = nullptr;
            char path[MAX_PATH] = "?";
            constexpr DWORD kFlags =
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
            if (GetModuleHandleExA(kFlags, static_cast<LPCSTR>(frames[i]), &module))
                GetModuleFileNameA(module, path, MAX_PATH);
            const char* base = std::strrchr(path, '\\');
            base = base != nullptr ? base + 1 : path;
            std::snprintf(line, sizeof line, "  #%-2d %s+0x%llx\n", i, base,
                          static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(frames[i])
                                                          - reinterpret_cast<std::uintptr_t>(module)));
            say(line);
        }
    }

    // The same frames named, from the PDBs beside the modules. `fromContext`: the first frame is where it failed, the
    // others return addresses (looked up a byte before, the call's own line).
    void printNamed(void* const* frames, int n, bool fromContext) noexcept
    {
        const HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS
                      | SYMOPT_NO_PROMPTS);
        if (!SymInitialize(process, nullptr, TRUE))
        {
            say("  (no symbols: SymInitialize failed)\n");
            return;
        }
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 512];
        char line[1200];
        for (int i = 0; i < n; ++i)
        {
            const DWORD64 address = reinterpret_cast<DWORD64>(frames[i]) - (fromContext && i == 0 ? 0 : 1);
            std::memset(storage, 0, sizeof storage);
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            const bool named = SymFromAddr(process, address, &displacement, symbol) != FALSE;
            IMAGEHLP_LINE64 where{};
            where.SizeOfStruct = sizeof where;
            DWORD column = 0;
            const bool lined = SymGetLineFromAddr64(process, address, &column, &where) != FALSE;
            if (lined)
                std::snprintf(line, sizeof line, "  #%-2d %s  %s:%lu\n", i, named ? symbol->Name : "?", where.FileName,
                              where.LineNumber);
            else
                std::snprintf(line, sizeof line, "  #%-2d %s\n", i, named ? symbol->Name : "?");
            say(line);
        }
    }

    void report(const char* what, const CONTEXT* context) noexcept
    {
        if (gReporting.exchange(true))                   // once: a crash while reporting ends the process its own way
            return;
        void* frames[64];
        const int n = walk(context, frames, 64);
        char head[400];
        std::snprintf(head, sizeof head, "\nCRASH  %s, on thread %lu\n", what, GetCurrentThreadId());
        say(head);
        printRaw(frames, n);
        say("CRASH  the same stack, named:\n");
        printNamed(frames, n, context != nullptr);
    }

    // Heap corruption reaches no unhandled-exception filter (the heap raises it where only a vectored handler sees it).
    LONG CALLBACK onVectored(EXCEPTION_POINTERS* e)
    {
        if (e->ExceptionRecord->ExceptionCode == 0xC0000374u)
        {
            char what[160];
            std::snprintf(what, sizeof what, "heap corruption (0xc0000374) found at %p",
                          e->ExceptionRecord->ExceptionAddress);
            report(what, e->ContextRecord);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG WINAPI onUnhandled(EXCEPTION_POINTERS* e)
    {
        const EXCEPTION_RECORD& r = *e->ExceptionRecord;
        char what[200];
        if (r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2)
        {
            const ULONG_PTR how = r.ExceptionInformation[0];
            std::snprintf(what, sizeof what, "access violation (0xc0000005) %s %p at %p",
                          how == 0 ? "reading" : how == 1 ? "writing" : "executing",
                          reinterpret_cast<void*>(r.ExceptionInformation[1]), r.ExceptionAddress);
        }
        else
            std::snprintf(what, sizeof what, "exception 0x%08lx at %p", static_cast<unsigned long>(r.ExceptionCode),
                          r.ExceptionAddress);
        report(what, e->ContextRecord);
        return EXCEPTION_CONTINUE_SEARCH;                // the process ends as it would have
    }

    void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, std::uintptr_t)
    {
        report("the C runtime's invalid-parameter check (a descriptor that is none, a null pointer, a small buffer)",
               nullptr);
        TerminateProcess(GetCurrentProcess(), 0xC0000417u);   // STATUS_INVALID_CRUNTIME_PARAMETER
    }

    void onPureCall()
    {
        report("a pure virtual function called (an object used while it is destroyed)", nullptr);
        TerminateProcess(GetCurrentProcess(), 0xC0000025u);   // STATUS_NONCONTINUABLE_EXCEPTION
    }

    void onAbort(int)
    {
        report("abort()", nullptr);                      // then the C runtime ends the process (exit code 3)
    }

    void onTerminate()
    {
        report("std::terminate (an exception escaping a noexcept function or a thread's body)", nullptr);
        std::abort();
    }

    void installCrashReport()
    {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);   // no window waits for a person to close it
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        AddVectoredExceptionHandler(1, onVectored);
        SetUnhandledExceptionFilter(onUnhandled);
        _set_invalid_parameter_handler(onInvalidParameter);
        _set_purecall_handler(onPureCall);
        std::signal(SIGABRT, onAbort);
        std::set_terminate(onTerminate);
    }
#endif
}

int main(int argc, char** argv)
{
#if defined(_WIN32)
    installCrashReport();
#endif
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
#if defined(__x86_64__) || defined(_M_X64)
        extra.emplace_back("x86_64");
#else
        extra.emplace_back("arm64");
#endif
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
