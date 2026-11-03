// Tools/probes/common/win/unistd.h: the few POSIX calls the probes and FunkGui's harness (funkgui/test/Harness.h,
// which includes <unistd.h>) make, over the Windows C runtime. On the probes' include path on Windows only
// (cmake/Probes.cmake): Windows has no <unistd.h> of its own.
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

// getpid: <process.h>'s own (the C runtime's POSIX name).
