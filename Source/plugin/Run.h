// Source/plugin/Run.h: a program started from an argument list and waited for, its output read back (JUCE-free; macOS,
// Windows and Linux). Never a shell between the caller and a path: args[0] is the program (a path, or a name looked up
// in PATH by this code, never the shell's), every other argument goes to it as it is. What git's helpers (plugin/Git.h)
// run goes through it; plugin/Launch.h stays the way to start a program and not wait (Linux's file manager).
//
// The child holds none of this process's descriptors or handles but its own three (a host's files, sockets and pipes
// stay the host's): on macOS POSIX_SPAWN_CLOEXEC_DEFAULT, on Linux glibc's posix_spawn_file_actions_addclosefrom_np,
// on Windows STARTUPINFOEX's handle list (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), as the MCP server starts its helper. Its
// stdin is the null device; its stdout and stderr are read into strings as it writes them, each kept to a limit (the
// rest read and dropped, so the child never blocks on a full pipe).
//
// A timeout and a cancel flag (looked at every 20 ms or so) kill it, and with it what it started: on POSIX the child
// leads a process group of its own and the group gets SIGKILL; on Windows it runs in a job object that is terminated.
// After the child exits, what it started and left holding its output is waited for at most kDrainMs, then left.
// Calls may run on any thread, several at once; none touches the audio thread.
#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace playground::run
{
    // How long the output is still read after the child exits, for what it started that still holds the pipes.
    inline constexpr int kDrainMs = 500;

    struct Request
    {
        std::vector<std::string> args;                               // args[0]: the program; a name without a folder
                                                                     // is looked up in PATH (Windows: with .exe)
        std::string folder;                                          // its working folder; "" this process's
        std::vector<std::pair<std::string, std::string>> env;        // set over this process's environment
        std::vector<std::string> unset;                              // taken out of it
        std::size_t outLimit = 1 << 20;                              // stdout: its first bytes kept
        std::size_t errLimit = 64 << 10;                             // stderr: its last bytes kept (an error is last)
        int timeoutMs = 30000;                                       // <= 0: none
        const std::atomic<bool>* cancel = nullptr;                   // true: killed
    };

    enum class Ended
    {
        exited,       // it ended by itself: `code` is its exit code
        signalled,    // POSIX: a signal ended it (not ours); `code` is the signal's number
        timedOut,     // killed: the timeout passed
        cancelled,    // killed: the cancel flag was set
        notStarted    // it never ran (not found, not a program, no pipe): `why` says
    };

    struct Result
    {
        Ended ended = Ended::notStarted;
        int code = -1;
        std::string out, err;
        bool outCut = false, errCut = false;                         // more was written than its limit kept
        std::string why;                                             // notStarted: why, in words
        double ms = 0.0;                                             // from the start to the end, wall clock

        bool ok() const { return ended == Ended::exited && code == 0; }
    };

    Result run(const Request& request);

    // The program a name stands for: as it is when it names a folder, else the first runnable file of that name in
    // PATH (Windows: the name, or the name with .exe); "" when there is none.
    std::string resolve(const std::string& name);

    // In words: "exited with 1", "timed out after 30 s", ...
    std::string describe(const Result& result, int timeoutMs);
}
