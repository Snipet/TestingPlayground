// Source/editor/nogpu/funkgui/CLocaleWin.cpp: FunkGui's funkgui/core/CLocale.h on Windows, in place of FunkGui's own
// src/core/CLocale.cpp, which uses POSIX's locale_t and does not compile there (FunkGui v0.23.1 builds on macOS and
// Linux). cmake/FunkGuiWindows.cmake swaps the two in FunkGui::core's sources on Windows only; this goes when FunkGui
// carries its own. The same contract: numbers parsed and printed in the C locale whatever the process's is, through
// the C runtime's _l functions over one C locale made once (never freed: a plug-in can be unloaded while other
// threads still format).
#include <funkgui/core/CLocale.h>

#include <cstdarg>
#include <cstdlib>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

namespace funkgui
{
    namespace
    {
        _locale_t cLocale() noexcept
        {
            static const _locale_t c = _create_locale(LC_ALL, "C");
            return c;
        }
    }

    float strtofC(const char* s, char** end) noexcept
    {
        return _strtof_l(s, end, cLocale());
    }

    long strtolC(const char* s, char** end, int base) noexcept
    {
        return _strtol_l(s, end, base, cLocale());
    }

    int snprintfC(char* buf, std::size_t size, const char* format, ...) noexcept
    {
        // C99's vsnprintf rules (the length it would have written, the buffer always terminated), in the C locale.
        if (buf != nullptr && size > 0)
        {
            va_list args;
            va_start(args, format);
            const int n = _vsnprintf_s_l(buf, size, _TRUNCATE, format, cLocale(), args);
            va_end(args);
            if (n >= 0)
                return n;
        }
        va_list again;
        va_start(again, format);
        const int whole = _vscprintf_l(format, cLocale(), again);
        va_end(again);
        return whole;
    }

    int fprintfC(std::FILE* f, const char* format, ...) noexcept
    {
        va_list args;
        va_start(args, format);
        const int n = _vfprintf_l(f, format, cLocale(), args);
        va_end(args);
        return n;
    }
}
