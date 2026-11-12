// Source/plugin/Run.cpp: see Run.h. Everything the child needs (its path, its arguments, its environment, its
// descriptors) is made before it starts; the child itself is posix_spawn's (or CreateProcessW's), never a fork of a
// host's threads that would then run code of ours.
#include "plugin/Run.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#if defined(_WIN32)
#include <cwchar>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ;
#endif
#endif

namespace playground::run
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

#if defined(_WIN32)
        std::wstring wide(const std::string& s)
        {
            if (s.empty())
                return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(static_cast<std::size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
            return w;
        }
#endif

        double msSince(Clock::time_point from)
        {
            return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
        }

        // What a stream keeps of what it reads: stdout its first bytes, stderr its last.
        struct Kept
        {
            std::string text;
            std::size_t limit = 0;
            bool tail = false;
            bool cut = false;

            void add(const char* data, std::size_t n)
            {
                if (!tail)
                {
                    const std::size_t room = text.size() < limit ? limit - text.size() : 0;
                    text.append(data, std::min(room, n));
                    cut = cut || n > room;
                    return;
                }
                text.append(data, n);
                if (text.size() > limit * 2 + 4096)
                    trim();
            }

            void trim()
            {
                if (tail && text.size() > limit)
                {
                    text.erase(0, text.size() - limit);
                    cut = true;
                }
            }
        };

        // NAME=value's name, as the environment compares it (Windows: case aside).
        std::string nameOf(const std::string& entry)
        {
            // Windows keeps entries like "=C:=C:\x" (a drive's folder): their name starts after the first '='.
            const std::size_t eq = entry.find('=', entry.empty() ? 0 : 1);
            return eq == std::string::npos ? entry : entry.substr(0, eq);
        }

        bool sameName(const std::string& a, const std::string& b)
        {
#if defined(_WIN32)
            return a.size() == b.size()
                && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
                   });
#else
            return a == b;
#endif
        }

        // This process's environment with the request's changes, as NAME=value strings.
        std::vector<std::string> environmentFor(const std::vector<std::string>& current, const Request& r)
        {
            std::vector<std::string> out;
            for (const std::string& entry : current)
            {
                const std::string name = nameOf(entry);
                const auto named = [&](const std::string& n) { return sameName(n, name); };
                if (std::any_of(r.unset.begin(), r.unset.end(), named)
                    || std::any_of(r.env.begin(), r.env.end(), [&](const auto& kv) { return named(kv.first); }))
                    continue;
                out.push_back(entry);
            }
            for (const auto& [name, value] : r.env)
                out.push_back(name + "=" + value);
            return out;
        }

        std::vector<std::string> pathFolders()
        {
            std::vector<std::string> dirs;
#if defined(_WIN32)
            const char separator = ';';
            std::string all;
            if (const DWORD n = GetEnvironmentVariableA("PATH", nullptr, 0); n > 0)
            {
                all.resize(n);
                all.resize(GetEnvironmentVariableA("PATH", all.data(), n));
            }
#else
            const char separator = ':';
            const char* env = std::getenv("PATH");
            const std::string all = env != nullptr && *env != '\0' ? env : "/usr/local/bin:/usr/bin:/bin";
#endif
            for (std::size_t from = 0;;)
            {
                const std::size_t at = all.find(separator, from);
                std::string dir = all.substr(from, at == std::string::npos ? std::string::npos : at - from);
#if defined(_WIN32)
                dir.erase(std::remove(dir.begin(), dir.end(), '"'), dir.end());
#endif
                // An empty entry means the working folder to a shell: never searched here.
                if (!dir.empty() && std::filesystem::path(dir).is_absolute())
                    dirs.push_back(dir);
                if (at == std::string::npos)
                    return dirs;
                from = at + 1;
            }
        }

        bool runnable(const std::string& path)
        {
#if defined(_WIN32)
            std::error_code ec;
            return std::filesystem::is_regular_file(std::filesystem::path(wide(path)), ec);
#else
            struct stat st {};
            return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(path.c_str(), X_OK) == 0;
#endif
        }
    }

    std::string resolve(const std::string& name)
    {
        if (name.empty())
            return {};
#if defined(_WIN32)
        const bool hasFolder = name.find_first_of("/\\:") != std::string::npos;
#else
        const bool hasFolder = name.find('/') != std::string::npos;
#endif
        if (hasFolder)
            return runnable(name) ? name : std::string();
        for (const std::string& dir : pathFolders())
        {
#if defined(_WIN32)
            const std::string base = dir + (dir.back() == '\\' || dir.back() == '/' ? "" : "\\") + name;
            const bool hasExtension = name.find('.') != std::string::npos;
            for (const std::string& path : hasExtension ? std::vector<std::string>{ base } : std::vector<std::string>{ base + ".exe" })
                if (runnable(path))
                    return path;
#else
            if (const std::string path = dir + "/" + name; runnable(path))
                return path;
#endif
        }
        return {};
    }

    std::string describe(const Result& result, int timeoutMs)
    {
        switch (result.ended)
        {
            case Ended::exited: return "exited with " + std::to_string(result.code);
            case Ended::signalled: return "was ended by signal " + std::to_string(result.code);
            case Ended::timedOut:
                return "took longer than " + std::to_string((timeoutMs + 999) / 1000) + " s and was stopped";
            case Ended::cancelled: return "was cancelled";
            case Ended::notStarted: return "could not start (" + result.why + ")";
        }
        return {};
    }

#if defined(_WIN32)
    namespace
    {
        std::string narrow(const std::wstring& w)
        {
            if (w.empty())
                return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<std::size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
            return s;
        }

        // One argument as CommandLineToArgvW (and the C runtime) reads it back: quoted, backslashes before a quote
        // doubled (Shaders.cpp's rule).
        std::wstring quoted(const std::wstring& a)
        {
            std::wstring out = L"\"";
            std::size_t slashes = 0;
            for (const wchar_t c : a)
            {
                if (c == L'\\')
                    ++slashes;
                else
                {
                    if (c == L'"')
                        out.append(slashes + 1, L'\\');
                    slashes = 0;
                }
                out += c;
            }
            out.append(slashes, L'\\');
            return out + L"\"";
        }

        std::vector<std::string> currentEnvironment()
        {
            std::vector<std::string> out;
            wchar_t* block = GetEnvironmentStringsW();
            if (block == nullptr)
                return out;
            for (const wchar_t* p = block; *p != L'\0'; p += std::wcslen(p) + 1)
                out.push_back(narrow(p));
            FreeEnvironmentStringsW(block);
            return out;
        }

        std::string windowsError(DWORD code)
        {
            return "Windows error " + std::to_string(code);
        }
    }

    Result run(const Request& r)
    {
        Result res;
        const auto start = Clock::now();
        if (r.args.empty() || r.args[0].empty())
        {
            res.why = "nothing to run";
            return res;
        }
        const std::string program = resolve(r.args[0]);
        if (program.empty())
        {
            res.why = r.args[0] + " was not found";
            return res;
        }
        std::error_code ec;
        if (!r.folder.empty() && !std::filesystem::is_directory(std::filesystem::path(wide(r.folder)), ec))
        {
            res.why = "no folder " + r.folder;
            return res;
        }

        std::wstring line;
        for (const std::string& a : r.args)
            line += (line.empty() ? L"" : L" ") + quoted(wide(&a == &r.args[0] ? program : a));

        // The environment block: NAME=value\0 ... \0, sorted by name, case aside, as CreateProcess asks.
        std::vector<std::string> env = environmentFor(currentEnvironment(), r);
        std::sort(env.begin(), env.end(), [](const std::string& a, const std::string& b) {
            return _stricmp(nameOf(a).c_str(), nameOf(b).c_str()) < 0;
        });
        std::wstring envBlock;
        for (const std::string& e : env)
        {
            envBlock += wide(e);
            envBlock += L'\0';
        }
        envBlock += L'\0';

        SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
        if (!CreatePipe(&outRead, &outWrite, &sa, 0))
        {
            res.why = "no pipe (" + windowsError(GetLastError()) + ")";
            return res;
        }
        if (!CreatePipe(&errRead, &errWrite, &sa, 0))
        {
            CloseHandle(outRead);
            CloseHandle(outWrite);
            res.why = "no pipe (" + windowsError(GetLastError()) + ")";
            return res;
        }
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
        HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);

        // The job: what the child starts goes with it when it is stopped (git's ssh, its remote helpers).
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job != nullptr)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        }

        // Only these handles are inherited (the handle list), whatever else the process holds open.
        std::vector<HANDLE> listed{ outWrite, errWrite };
        if (in != INVALID_HANDLE_VALUE)
            listed.push_back(in);
        SIZE_T attrBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrBytes);
        std::vector<unsigned char> attrBuffer(attrBytes);
        auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer.data());
        BOOL started = FALSE;
        DWORD failure = 0;
        PROCESS_INFORMATION pi{};
        if (attrBytes > 0 && InitializeProcThreadAttributeList(attrs, 1, 0, &attrBytes))
        {
            if (UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, listed.data(),
                                          listed.size() * sizeof(HANDLE), nullptr, nullptr))
            {
                STARTUPINFOEXW si{};
                si.StartupInfo.cb = sizeof si;
                si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
                si.StartupInfo.hStdInput = in != INVALID_HANDLE_VALUE ? in : nullptr;
                si.StartupInfo.hStdOutput = outWrite;
                si.StartupInfo.hStdError = errWrite;
                si.lpAttributeList = attrs;
                const std::wstring exe = wide(program);
                const std::wstring dir = wide(r.folder);
                started = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE,
                                         CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT
                                             | EXTENDED_STARTUPINFO_PRESENT,
                                         envBlock.data(), dir.empty() ? nullptr : dir.c_str(), &si.StartupInfo, &pi);
                if (!started)
                    failure = GetLastError();
            }
            else
                failure = GetLastError();
            DeleteProcThreadAttributeList(attrs);
        }
        CloseHandle(outWrite);
        CloseHandle(errWrite);
        if (in != INVALID_HANDLE_VALUE)
            CloseHandle(in);
        if (!started)
        {
            CloseHandle(outRead);
            CloseHandle(errRead);
            if (job != nullptr)
                CloseHandle(job);
            res.why = r.args[0] + " could not start (" + windowsError(failure) + ")";
            return res;
        }
        if (job != nullptr)
            AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);

        Kept out{ {}, r.outLimit, false, false }, err{ {}, r.errLimit, true, false };
        const auto reader = [](HANDLE h, Kept* k) {
            char buffer[8192];
            DWORD n = 0;
            while (ReadFile(h, buffer, sizeof buffer, &n, nullptr) && n > 0)
                k->add(buffer, n);
        };
        std::thread outThread(reader, outRead, &out), errThread(reader, errRead, &err);

        const auto stop = [&] {
            if (job != nullptr)
                TerminateJobObject(job, 1);
            else
                TerminateProcess(pi.hProcess, 1);
        };
        for (;;)
        {
            if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0)
            {
                DWORD code = 0;
                GetExitCodeProcess(pi.hProcess, &code);
                if (res.ended == Ended::notStarted)
                {
                    res.ended = Ended::exited;
                    res.code = static_cast<int>(code);
                }
                break;
            }
            if (r.cancel != nullptr && r.cancel->load(std::memory_order_relaxed))
            {
                res.ended = Ended::cancelled;
                stop();
            }
            else if (r.timeoutMs > 0 && msSince(start) > r.timeoutMs)
            {
                res.ended = Ended::timedOut;
                stop();
            }
        }
        // What it started may still hold the pipes: the job's go now, and a reader still blocked after kDrainMs (a
        // process outside the job) is cancelled.
        const auto exited = Clock::now();
        if (job != nullptr)
            TerminateJobObject(job, 0);
        for (std::thread* t : { &outThread, &errThread })
        {
            while (WaitForSingleObject(static_cast<HANDLE>(t->native_handle()), 10) == WAIT_TIMEOUT)
                if (msSince(exited) > kDrainMs)
                    CancelSynchronousIo(static_cast<HANDLE>(t->native_handle()));
            t->join();
        }
        CloseHandle(outRead);
        CloseHandle(errRead);
        CloseHandle(pi.hProcess);
        if (job != nullptr)
            CloseHandle(job);
        err.trim();
        res.out = std::move(out.text);
        res.err = std::move(err.text);
        res.outCut = out.cut;
        res.errCut = err.cut;
        res.ms = msSince(start);
        return res;
    }

#else
    namespace
    {
        char** currentEnviron()
        {
#if defined(__APPLE__)
            return *_NSGetEnviron();
#else
            return environ;
#endif
        }

        // A descriptor at 3 or above, close-on-exec (Launch.cpp's rule): a host that closed its stdin, stdout or stderr
        // would hand out one of those numbers, and the child's dup2 onto it would do nothing (and leave it closed).
        int aboveStdio(int fd)
        {
            if (fd < 0 || fd > 2)
                return fd;
            const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
            ::close(fd);
            return moved;
        }

        bool cloexecPipe(int fds[2])
        {
#if defined(__linux__)
            if (::pipe2(fds, O_CLOEXEC) != 0)
                return false;
#else
            if (::pipe(fds) != 0)
                return false;
            ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
            ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
            fds[0] = aboveStdio(fds[0]);
            fds[1] = aboveStdio(fds[1]);
            return fds[0] >= 0 && fds[1] >= 0;
        }
    }

    Result run(const Request& r)
    {
        Result res;
        const auto start = Clock::now();
        if (r.args.empty() || r.args[0].empty())
        {
            res.why = "nothing to run";
            return res;
        }
        const std::string program = resolve(r.args[0]);
        if (program.empty())
        {
            res.why = r.args[0] + " was not found";
            return res;
        }
        if (!r.folder.empty())
        {
            struct stat st {};
            if (::stat(r.folder.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
            {
                res.why = "no folder " + r.folder;
                return res;
            }
        }

        std::vector<std::string> current;
        for (char** e = currentEnviron(); e != nullptr && *e != nullptr; ++e)
            current.emplace_back(*e);
        const std::vector<std::string> env = environmentFor(current, r);
        std::vector<char*> envp;
        envp.reserve(env.size() + 1);
        for (const std::string& e : env)
            envp.push_back(const_cast<char*>(e.c_str()));
        envp.push_back(nullptr);
        std::vector<char*> argv;
        argv.reserve(r.args.size() + 1);
        for (const std::string& a : r.args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        int outPipe[2] = { -1, -1 }, errPipe[2] = { -1, -1 };
        const auto closeAll = [&] {
            for (int* fd : { &outPipe[0], &outPipe[1], &errPipe[0], &errPipe[1] })
                if (*fd >= 0)
                {
                    ::close(*fd);
                    *fd = -1;
                }
        };
        if (!cloexecPipe(outPipe) || !cloexecPipe(errPipe))
        {
            res.why = std::string("no pipe (") + std::strerror(errno) + ")";
            closeAll();
            return res;
        }

        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&fa, outPipe[1], 1);
        posix_spawn_file_actions_adddup2(&fa, errPipe[1], 2);
        if (!r.folder.empty())
            posix_spawn_file_actions_addchdir_np(&fa, r.folder.c_str());
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
        flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;        // macOS: only 0, 1 and 2 (the file actions' targets) reach it
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
        posix_spawn_file_actions_addclosefrom_np(&fa, 3);   // Linux: whatever the host left without close-on-exec
#endif
        posix_spawnattr_setflags(&attr, flags);
        posix_spawnattr_setpgroup(&attr, 0);         // a group of its own: a stop reaches what it started
        sigset_t none, defaults;
        sigemptyset(&none);
        posix_spawnattr_setsigmask(&attr, &none);
        sigemptyset(&defaults);
        sigaddset(&defaults, SIGPIPE);               // a host that ignores SIGPIPE: the child gets the default
        posix_spawnattr_setsigdefault(&attr, &defaults);

        pid_t pid = -1;
        const int spawned = ::posix_spawn(&pid, program.c_str(), &fa, &attr, argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&attr);
        ::close(outPipe[1]);
        ::close(errPipe[1]);
        outPipe[1] = errPipe[1] = -1;
        if (spawned != 0)
        {
            res.why = r.args[0] + " could not start (" + std::strerror(spawned) + ")";
            closeAll();
            return res;
        }
        for (const int fd : { outPipe[0], errPipe[0] })
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);

        Kept out{ {}, r.outLimit, false, false }, err{ {}, r.errLimit, true, false };
        bool reaped = false;
        Clock::time_point reapedAt{};
        const auto stop = [&](Ended why) {
            if (res.ended == Ended::notStarted)
                res.ended = why;
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
        };
        for (;;)
        {
            pollfd fds[2];
            nfds_t count = 0;
            for (const int fd : { outPipe[0], errPipe[0] })
                if (fd >= 0)
                    fds[count++] = { fd, POLLIN, 0 };
            if (count > 0)
                (void)::poll(fds, count, 20);
            else if (!reaped)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            for (int* fd : { &outPipe[0], &errPipe[0] })
            {
                Kept& kept = fd == &outPipe[0] ? out : err;
                for (;;)
                {
                    if (*fd < 0)
                        break;
                    char buffer[16384];
                    const ssize_t n = ::read(*fd, buffer, sizeof buffer);
                    if (n > 0)
                    {
                        kept.add(buffer, static_cast<std::size_t>(n));
                        continue;
                    }
                    if (n < 0 && errno == EINTR)
                        continue;
                    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                    {
                        ::close(*fd);
                        *fd = -1;
                    }
                    break;
                }
            }
            if (!reaped)
            {
                int status = 0;
                const pid_t done = ::waitpid(pid, &status, WNOHANG);
                if (done == pid)
                {
                    reaped = true;
                    reapedAt = Clock::now();
                    // A process group's leader is gone; what it started may still run: the group is stopped too.
                    if (res.ended == Ended::notStarted)
                    {
                        if (WIFEXITED(status))
                        {
                            res.ended = Ended::exited;
                            res.code = WEXITSTATUS(status);
                        }
                        else if (WIFSIGNALED(status))
                        {
                            res.ended = Ended::signalled;
                            res.code = WTERMSIG(status);
                        }
                    }
                    else
                        ::kill(-pid, SIGKILL);
                }
                else if (done < 0 && errno != EINTR)
                {
                    reaped = true;
                    reapedAt = Clock::now();
                    if (res.ended == Ended::notStarted)
                        res.ended = Ended::signalled;
                }
                else if (r.cancel != nullptr && r.cancel->load(std::memory_order_relaxed))
                    stop(Ended::cancelled);
                else if (r.timeoutMs > 0 && msSince(start) > r.timeoutMs)
                    stop(Ended::timedOut);
            }
            if (reaped && (outPipe[0] < 0 && errPipe[0] < 0))
                break;
            if (reaped && std::chrono::duration<double, std::milli>(Clock::now() - reapedAt).count() > kDrainMs)
                break;   // what it started holds the pipes on: its output is left unread
        }
        closeAll();
        err.trim();
        res.out = std::move(out.text);
        res.err = std::move(err.text);
        res.outCut = out.cut;
        res.errCut = err.cut;
        res.ms = msSince(start);
        return res;
    }
#endif
}
