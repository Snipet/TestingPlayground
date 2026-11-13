// Source/plugin/FileReplace.cpp: see FileReplace.h.
#include "plugin/FileReplace.h"

#if defined(_WIN32)
#include <windows.h>

#include <chrono>
#include <thread>
#endif

namespace playground::files
{
    bool replace(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec)
    {
        ec.clear();
#if defined(_WIN32)
        // std::filesystem::path holds the wide string Windows takes.
        const std::wstring f = from.wstring(), t = to.wstring();
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
        for (;;)
        {
            if (MoveFileExW(f.c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING) != 0)
            {
                ec.clear();                              // a try before may have failed: a success says nothing of it
                return true;
            }
            const DWORD e = GetLastError();
            ec = std::error_code(static_cast<int>(e), std::system_category());
            const bool held = e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED || e == ERROR_LOCK_VIOLATION;
            if (!held || std::chrono::steady_clock::now() >= until)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryMs));
        }
#else
        std::filesystem::rename(from, to, ec);
        return !ec;
#endif
    }
}
