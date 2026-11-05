// Tools/probes/common/win/unistd.h: the few POSIX calls the probes make (getpid, setenv, unsetenv), over the Windows
// C runtime, which has no <unistd.h>. Windows only, on the probes' include path alone (cmake/Probes.cmake): never on a
// plugin target's, where a library that tests for <unistd.h> would take it for POSIX. (FunkGui's harness needs none
// since FunkGui v0.24.0.)
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
