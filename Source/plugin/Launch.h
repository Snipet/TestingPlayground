// Source/plugin/Launch.h: a program started from an argument list, never through a shell (JUCE-free; POSIX: macOS and
// Linux). Linux's Show in File Manager and Open in VS Code (Processor::openFolder) go through it: JUCE's
// Process::openDocument there runs "xdg-open \"<path>\" || ..." with /bin/sh -c, where a folder's name holding $(...)
// or a backtick would run as a command, and leaves its child a zombie of the host. macOS opens through NSWorkspace,
// Windows through ShellExecute (no shell either); the probes run it on both POSIX systems (plugin.folder LAUNCH).
#pragma once

#if !defined(_WIN32)

#include <string>
#include <vector>

namespace playground::launch
{
    // What opens a folder in the desktop's own file manager on Linux (or a URL in its handler: vscode://...):
    // xdg-open, given the path as one argument as it is.
    std::vector<std::string> openerArgs(const std::string& pathOrUrl);

    // Starts args[0] (looked up in PATH when it names no folder) with `args` as its argument list, detached: in a
    // session of its own, stdin and stdout /dev/null, stderr the host's, no other descriptor of the host's kept, its
    // signal mask cleared. An intermediate child starts it and ends at once, so it is never this process's child
    // (init reaps it) and the call returns as soon as it has started, however long it runs. False with why when it
    // could not start (not found, not executable, the exec failed); whether it then did what it was asked is not known.
    bool detached(const std::vector<std::string>& args, std::string& why);
}

#endif
