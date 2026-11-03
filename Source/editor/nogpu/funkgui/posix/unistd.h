// Source/editor/nogpu/funkgui/posix/unistd.h: the few POSIX calls FunkGui's test harness (funkgui/test/Harness.h,
// which includes <unistd.h>) and the probes make, over the Windows C runtime, which has no <unistd.h>. Windows only, and
// only where something includes the harness: cmake/FunkGuiWindows.cmake puts this folder on the include path of each
// FunkGui source that does (src/canvas/Fingerprint.cpp), and cmake/Probes.cmake on the probes'. Never on a plugin
// target's whole path: a library that tests for <unistd.h> would take it for POSIX.
#pragma once

#include <direct.h>
#include <io.h>
#include <process.h>
#include <stdlib.h>

inline int setenv(const char* name, const char* value, int overwrite)
{
    if (overwrite == 0 && getenv(name) != nullptr)
        return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}

inline int unsetenv(const char* name)
{
    return _putenv_s(name, "") == 0 ? 0 : -1;       // an empty value removes the variable
}

// The C runtime's own name for it (its POSIX name, getpid, is declared only under some of its settings).
#ifndef getpid
#define getpid _getpid
#endif
