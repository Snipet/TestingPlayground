// Source/installer/SystemWin.cpp: the engine's system tools on Windows (Engine.h's WinSystem): robocopy, the process
// list, the Recycle Bin (SHFileOperation, which asks before it deletes anything it cannot recycle) and the administrator
// prompt (UAC: ShellExecuteEx "runas" of System32's PowerShell). Every program runs without a shell, its arguments
// quoted as the C runtime reads them back, inheriting no handle but its own three; the one script is PowerShell's, its
// text passed as -EncodedCommand, never a file.
#include "installer/Engine.h"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <objbase.h>
#undef near                                          // windows.h's 16-bit leftovers
#undef far

#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace playground::installer
{
    namespace
    {
        std::wstring wide(const std::string& s)
        {
            if (s.empty())
                return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(static_cast<std::size_t>(std::max(n, 0)), L'\0');
            if (n > 0)
                MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
            return w;
        }

        std::string utf8(const std::wstring& w)
        {
            if (w.empty())
                return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<std::size_t>(std::max(n, 0)), '\0');
            if (n > 0)
                WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
            return s;
        }

        std::string pathText(const fs::path& p)
        {
            return utf8(p.wstring());
        }

        std::wstring systemDir()
        {
            wchar_t b[MAX_PATH];
            const UINT n = GetSystemDirectoryW(b, MAX_PATH);
            return n > 0 && n < MAX_PATH ? std::wstring(b, n) : std::wstring(L"C:\\Windows\\System32");
        }

        std::string trimmedText(std::string s)
        {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.'))
                s.pop_back();
            std::size_t a = 0;
            while (a < s.size() && (s[a] == '\n' || s[a] == '\r' || s[a] == ' '))
                ++a;
            return s.substr(a);
        }

        std::string errorWords(DWORD e)
        {
            wchar_t* text = nullptr;
            const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                           nullptr, e, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
            std::string s = n > 0 && text != nullptr ? utf8(std::wstring(text, n)) : "error " + std::to_string(e);
            LocalFree(text);
            return trimmedText(s);
        }

        // An argument as CommandLineToArgvW and the C runtime read it back: quoted when it holds a space, a tab or a
        // quote, a quote escaped, and the backslashes before a quote (or the end) doubled.
        std::wstring quoted(const std::wstring& a)
        {
            if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos)
                return a;
            std::wstring q = L"\"";
            for (std::size_t i = 0;; ++i)
            {
                std::size_t slashes = 0;
                while (i < a.size() && a[i] == L'\\')
                {
                    ++slashes;
                    ++i;
                }
                if (i == a.size())
                {
                    q.append(slashes * 2, L'\\');
                    break;
                }
                if (a[i] == L'"')
                {
                    q.append(slashes * 2 + 1, L'\\');
                    q.push_back(L'"');
                }
                else
                {
                    q.append(slashes, L'\\');
                    q.push_back(a[i]);
                }
            }
            q.push_back(L'"');
            return q;
        }

        void drain(HANDLE h, std::string& to)
        {
            char buf[4096];
            DWORD n = 0;
            while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0)
                to.append(buf, n);
        }

        // The window to own a prompt or a dialog: this process's visible top-level one, if any.
        HWND ownWindow()
        {
            struct Find
            {
                DWORD pid;
                HWND found;
            } f{ GetCurrentProcessId(), nullptr };
            EnumWindows([](HWND w, LPARAM p) -> BOOL {
                auto* find = reinterpret_cast<Find*>(p);
                DWORD pid = 0;
                GetWindowThreadProcessId(w, &pid);
                if (pid == find->pid && IsWindowVisible(w) && GetWindow(w, GW_OWNER) == nullptr)
                {
                    find->found = w;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&f));
            return f.found;
        }

        // COM on this thread for the shell's calls, as long as the call: whatever apartment it already has is kept.
        struct ComScope
        {
            bool ours = false;
            ComScope() { ours = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)); }
            ~ComScope()
            {
                if (ours)
                    CoUninitialize();
            }
        };

        std::string base64(const unsigned char* data, std::size_t n)
        {
            static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve((n + 2) / 3 * 4);
            for (std::size_t i = 0; i < n; i += 3)
            {
                const uint32_t b0 = data[i], b1 = i + 1 < n ? data[i + 1] : 0u, b2 = i + 2 < n ? data[i + 2] : 0u;
                const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
                out += kTable[(v >> 18) & 63];
                out += kTable[(v >> 12) & 63];
                out += i + 1 < n ? kTable[(v >> 6) & 63] : '=';
                out += i + 2 < n ? kTable[v & 63] : '=';
            }
            return out;
        }
    }

    int runProgram(const std::vector<std::string>& argv, std::string* out, std::string* err)
    {
        if (argv.empty())
            return -1;
        std::wstring cmd;
        for (const std::string& a : argv)
            cmd += (cmd.empty() ? L"" : L" ") + quoted(wide(a));
        SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
        HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
        if (!CreatePipe(&outR, &outW, &sa, 0))
            return -1;
        if (!CreatePipe(&errR, &errW, &sa, 0))
        {
            CloseHandle(outR);
            CloseHandle(outW);
            return -1;
        }
        SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        // The child inherits these three alone (STARTUPINFOEX's handle list), whatever else this process holds.
        HANDLE inherit[3] = { nul, outW, errW };
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<unsigned char> attrsBuf(size);
        auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrsBuf.data());
        const bool listed = InitializeProcThreadAttributeList(attrs, 1, 0, &size)
                            && UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                                         sizeof inherit, nullptr, nullptr);
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nul;
        si.StartupInfo.hStdOutput = outW;
        si.StartupInfo.hStdError = errW;
        si.lpAttributeList = listed ? attrs : nullptr;
        PROCESS_INFORMATION pi{};
        const std::wstring app = wide(argv[0]);
        // A program named by its path is run as it is; a bare name is looked for as the system looks for one.
        const bool path = app.find_first_of(L"\\/") != std::wstring::npos;
        const BOOL started = CreateProcessW(path ? app.c_str() : nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT
                                                | (listed ? EXTENDED_STARTUPINFO_PRESENT : 0u),
                                            nullptr, nullptr, &si.StartupInfo, &pi);
        const DWORD startError = started ? 0 : GetLastError();
        if (listed)
            DeleteProcThreadAttributeList(attrs);
        CloseHandle(outW);
        CloseHandle(errW);
        if (nul != INVALID_HANDLE_VALUE)
            CloseHandle(nul);
        if (!started)
        {
            CloseHandle(outR);
            CloseHandle(errR);
            if (err != nullptr)
                *err = "could not start " + argv[0] + " (" + errorWords(startError) + ")";
            return -1;
        }
        // Both pipes read until they close, so that neither fills and stops the program.
        std::string o, e;
        std::thread readErr([&] { drain(errR, e); });
        drain(outR, o);
        readErr.join();
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(outR);
        CloseHandle(errR);
        if (out != nullptr)
            *out = o;
        if (err != nullptr)
            *err = e;
        return static_cast<int>(code);
    }

    bool copyItem(const fs::path& from, const fs::path& to, std::string& error)
    {
        const std::string robocopy = utf8(systemDir() + L"\\robocopy.exe");
        std::error_code ec;
        const bool dir = fs::is_directory(fs::symlink_status(from, ec));
        std::vector<std::string> args;
        if (dir)
            args = { robocopy, pathText(from), pathText(to), "/E", "/SL", "/XJ", "/COPY:DAT", "/DCOPY:DAT", "/R:0", "/W:0",
                     "/NFL", "/NDL", "/NJH", "/NJS", "/NP" };
        else
            args = { robocopy, pathText(from.parent_path()), pathText(to.parent_path()), pathText(from.filename()), "/SL",
                     "/COPY:DAT", "/R:0", "/W:0", "/NFL", "/NDL", "/NJH", "/NJS", "/NP" };
        std::string out, err;
        const int status = runProgram(args, &out, &err);
        if (status >= 0 && status < 8)                   // robocopy: 0 to 7 is success, 8 and up a failure
        {
            // robocopy keeps a file's name: one copied under another name is renamed.
            if (!dir && from.filename() != to.filename()
                && !MoveFileExW((to.parent_path() / from.filename()).c_str(), to.c_str(), 0))
            {
                error = errorWords(GetLastError());
                return false;
            }
            return true;
        }
        const std::string said = trimmedText(err.empty() ? out : err);
        error = said.empty() ? "robocopy stopped with status " + std::to_string(status) : said;
        return false;
    }

    fs::path powershellExe()
    {
        return fs::path(systemDir()) / "WindowsPowerShell" / "v1.0" / "powershell.exe";
    }

    std::string psEncoded(const std::string& script)
    {
        const std::wstring w = wide(script);
        std::vector<unsigned char> bytes;
        bytes.reserve(w.size() * 2);
        for (const wchar_t c : w)
        {
            bytes.push_back(static_cast<unsigned char>(static_cast<uint16_t>(c) & 0xFF));
            bytes.push_back(static_cast<unsigned char>(static_cast<uint16_t>(c) >> 8));
        }
        return base64(bytes.data(), bytes.size());
    }

    bool WinSystem::copy(const fs::path& from, const fs::path& to, std::string& error)
    {
        return copyItem(from, to, error);
    }

    std::vector<fs::path> WinSystem::runningApps()
    {
        std::vector<fs::path> apps;
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return apps;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof pe;
        for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
        {
            const HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (p == nullptr)
                continue;                                // the system's own: not ours to name
            wchar_t buf[32768];
            DWORD n = static_cast<DWORD>(std::size(buf));
            if (QueryFullProcessImageNameW(p, 0, buf, &n))
                apps.emplace_back(std::wstring(buf, n));
            CloseHandle(p);
        }
        CloseHandle(snap);
        return apps;
    }

    bool WinSystem::restartAuRegistrar()
    {
        return true;                                     // no Audio Units on Windows
    }

    bool WinSystem::moveToTrash(const fs::path& path, std::string& error)
    {
        // FOF_WANTNUKEWARNING: anything the Recycle Bin cannot take (too large, a drive without one) is never deleted
        // without the user's yes in Windows' own warning.
        const ComScope com;
        std::wstring from = path.wstring();
        from.push_back(L'\0');                           // a list of one, ended by an empty name
        SHFILEOPSTRUCTW op{};
        op.hwnd = ownWindow();
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_WANTNUKEWARNING;
        const int r = SHFileOperationW(&op);
        if (r == 0 && !op.fAnyOperationsAborted)
            return true;
        error = op.fAnyOperationsAborted ? "it was left where it is, as asked" : "Windows would not move it to the Recycle Bin ("
                                                                                     + std::to_string(r) + ")";
        return false;
    }

    System::Admin WinSystem::runAsAdministrator(const std::string& script, const std::string& prompt, std::string& error,
                                                int& status)
    {
        // UAC shows its own words (Windows PowerShell, by Microsoft Windows): `prompt` is for the log. The script goes
        // to System32's PowerShell as an argument, encoded: no file of it waits anywhere while the prompt does.
        static_cast<void>(prompt);
        status = -1;
        const ComScope com;
        const std::wstring exe = powershellExe().wstring();
        const std::wstring params = L"-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden "
                                    L"-EncodedCommand " + wide(psEncoded(script));
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof sei;
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.hwnd = ownWindow();
        sei.lpVerb = L"runas";
        sei.lpFile = exe.c_str();
        sei.lpParameters = params.c_str();
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei))
        {
            const DWORD e = GetLastError();
            if (e == ERROR_CANCELLED)                    // the prompt said no
                return Admin::cancelled;
            error = errorWords(e);
            return Admin::failed;
        }
        if (sei.hProcess == nullptr)
        {
            error = "Windows started no process";
            return Admin::failed;
        }
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        status = static_cast<int>(code);
        return code == 0 ? Admin::ran : Admin::failed;
    }
}
