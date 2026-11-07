// Source/plugin/Launch.cpp: see Launch.h. Everything the children need (the program's path, the argument list, the
// descriptors) is made before the first fork: in a process with other threads (a host's) a child may call only what
// is async-signal-safe, so the children call nothing but system calls.
#include "plugin/Launch.h"

#if !defined(_WIN32)

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace playground::launch
{
    namespace
    {
        // A descriptor at 3 or above, close-on-exec: a host that closed its stdin, stdout or stderr would otherwise
        // hand one of those numbers out, and the child's dup2 onto 0 and 1 would replace it.
        int aboveStdio(int fd)
        {
            if (fd < 0 || fd > 2)
                return fd;
            const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
            ::close(fd);
            return moved;
        }

        // The program args[0] names: as it is when it names a folder, else the first executable file of that name
        // in PATH ("" when there is none).
        std::string resolve(const std::string& name)
        {
            const auto runnable = [](const std::string& path) {
                struct stat st {};
                return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(path.c_str(), X_OK) == 0;
            };
            if (name.find('/') != std::string::npos)
                return runnable(name) ? name : std::string();
            const char* env = std::getenv("PATH");
            const std::string dirs = env != nullptr && *env != '\0' ? env : "/usr/local/bin:/usr/bin:/bin";
            for (std::size_t from = 0;;)
            {
                const std::size_t colon = dirs.find(':', from);
                std::string dir = dirs.substr(from, colon == std::string::npos ? std::string::npos : colon - from);
                if (dir.empty())
                    dir = ".";
                if (const std::string path = dir + "/" + name; runnable(path))
                    return path;
                if (colon == std::string::npos)
                    return {};
                from = colon + 1;
            }
        }

        // In the grandchild: every descriptor from 3 up closed at the exec (the report pipe's end until then).
        void closeOnExecFrom3(int last)
        {
#if defined(__linux__)
            if (::close_range(3, ~0U, CLOSE_RANGE_CLOEXEC) == 0)
                return;
#endif
            for (int fd = 3; fd <= last; ++fd)
                (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }

        // The intermediate child's fork: glibc's _Fork runs no atfork handlers (async-signal-safe); macOS has none.
        pid_t forkInChild()
        {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
            return ::_Fork();
#else
            return ::fork();
#endif
        }

        void report(int fd, int error)
        {
            (void)!::write(fd, &error, sizeof error);
        }
    }

    std::vector<std::string> openerArgs(const std::string& pathOrUrl) { return { "xdg-open", pathOrUrl }; }

    bool detached(const std::vector<std::string>& args, std::string& why)
    {
        if (args.empty() || args[0].empty())
        {
            why = "nothing to start";
            return false;
        }
        const std::string program = resolve(args[0]);
        if (program.empty())
        {
            why = args[0] + " was not found";
            return false;
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const std::string& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        // The descriptors' last number, for the loop that marks them where close_range is missing.
        struct rlimit limit {};
        int last = 1023;
        if (::getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY)
            last = static_cast<int>(limit.rlim_cur < 65536 ? limit.rlim_cur : 65536) - 1;

        const int devNull = aboveStdio(::open("/dev/null", O_RDWR | O_CLOEXEC));
        int pipeFds[2] = { -1, -1 };
#if defined(__linux__)
        const bool piped = ::pipe2(pipeFds, O_CLOEXEC) == 0;
#else
        const bool piped = ::pipe(pipeFds) == 0 && ::fcntl(pipeFds[0], F_SETFD, FD_CLOEXEC) == 0
                        && ::fcntl(pipeFds[1], F_SETFD, FD_CLOEXEC) == 0;
#endif
        const int readEnd = piped ? aboveStdio(pipeFds[0]) : -1;
        const int writeEnd = piped ? aboveStdio(pipeFds[1]) : -1;
        const auto closeAll = [&] {
            for (const int fd : { devNull, readEnd, writeEnd })
                if (fd >= 0)
                    ::close(fd);
        };
        if (devNull < 0 || readEnd < 0 || writeEnd < 0)
        {
            const int error = errno;
            closeAll();
            why = args[0] + " could not start (" + std::strerror(error) + ")";
            return false;
        }

        const char* path = program.c_str();
        const pid_t child = ::fork();
        if (child < 0)
        {
            const int error = errno;
            closeAll();
            why = args[0] + " could not start (" + std::strerror(error) + ")";
            return false;
        }
        if (child == 0)
        {
            // The intermediate child: starts the program and ends, so the program is never the host's child.
            const pid_t grandchild = forkInChild();
            if (grandchild != 0)
            {
                if (grandchild < 0)
                    report(writeEnd, errno);
                ::_exit(grandchild < 0 ? 1 : 0);
            }
            ::setsid();
            ::dup2(devNull, 0);
            ::dup2(devNull, 1);
            closeOnExecFrom3(last);
            sigset_t none;
            (void)sigemptyset(&none);
            ::sigprocmask(SIG_SETMASK, &none, nullptr);
            struct sigaction dfl {};
            dfl.sa_handler = SIG_DFL;
            ::sigaction(SIGPIPE, &dfl, nullptr);           // a host that ignores SIGPIPE: the program gets the default
            ::execv(path, argv.data());
            report(writeEnd, errno);
            ::_exit(127);
        }

        ::close(writeEnd);
        ::close(devNull);
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR)
        {
        }
        // The report pipe ends when the program's exec closed its last writer; an errno on it means it never ran.
        int error = 0;
        ssize_t n = 0;
        while ((n = ::read(readEnd, &error, sizeof error)) < 0 && errno == EINTR)
        {
        }
        ::close(readEnd);
        if (n == static_cast<ssize_t>(sizeof error))
        {
            why = args[0] + " could not start (" + std::strerror(error) + ")";
            return false;
        }
        return true;
    }
}

#endif
