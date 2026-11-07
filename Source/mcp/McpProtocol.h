// Source/mcp/McpProtocol.h: how this playground's instances (plugin/McpLink.h) and its MCP server (mcp/Main.cpp, the
// program Claude Desktop starts) find and talk to each other.
//
//   the folder   one per user and plugin, private (0700): <the user's temp folder>/Funk-<plugin>-mcp/, or
//                <ENV_PREFIX>MCP_DIR when set (the probes' own). In it each instance's socket, <n>.sock: the lowest
//                number free when it started, its number's name to the user ("Synth 2", unless the user named it);
//                its card, <n>.json, and Claude Desktop's target, target.json (mcp/McpTarget.h); and what the newest
//                instance wrote for a server that finds none running: tools.json and guide.md
//   a request    one JSON object a line: { "id": 1, "method": "info" | "tools" | "guide" | "call" | "cancel",
//                "params": { ... } }; several may be in flight on one connection
//   a reply      one line: { "id": 1, "result": ... } or { "id": 1, "error": "why" }, exactly one a request, in the
//                order they are ready (a quick one may pass a long call's)
//   never stuck  both ends' sockets are non-blocking, and each end queues what it writes and keeps reading while it
//                waits to send the rest: a large request and a large reply crossing each other (more than a socket's
//                buffer, 8 KB on macOS) never leave both ends waiting for the other to read
//                  info    the instance: { id, name, kind, plugin, host, track, project, version, preset, edited,
//                          files, compiled, busy } (name: the user's for it, else its number's, "Synth 2")
//                  tools   the tools, as MCP lists them ({ name, title, description, inputSchema, annotations })
//                  guide   { part } -> a part of what the plugin's own chat works from, for an assistant that drives it
//                          from outside (mcp/McpGuide.h; an error for a part that is not). Without `part` (a 0.2.0
//                          server, which knows no parts): the whole guide
//                  call    { name, arguments } -> { content: [ MCP content ], isError }: one at a time, in order, but
//                          for the live reads (ChatAgent::mcpLive), which answer at once
//                  cancel  { id } -> { cancelled }: stop the call request `id` made on this connection (one waiting
//                          is answered "stopped before it started"; the one running stops as soon as it can, and
//                          still answers)
// Unix-domain sockets, no JUCE, so the server stays small. POSIX on macOS; on Windows (10 1803 and later) Winsock's
// AF_UNIX, which every call below reaches through the few functions of "the platform" (a socket is an int on both:
// Winsock's handles fit), and the folder is in the user's temp folder (%TEMP%, private to the user's profile). What is
// no socket on Windows (the server's stdin, the sandbox's helper's pipes) is bridged to one (bridgeFrom, bridgeTo), so
// that the server's one loop polls sockets alone there.
#pragma once

#include "Product.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <chrono>
#include <mutex>
#include <thread>
#undef near                                          // windows.h's 16-bit leftovers: plain names to every includer
#undef far
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace playground::mcp
{
    inline constexpr std::size_t kMaxLine = 64u * 1024u * 1024u;   // a reply with pictures stays well under

    // ---- the platform ------------------------------------------------------------------------------------------------
#if defined(_WIN32)
    // A poll() entry: Winsock's WSAPOLLFD holds a SOCKET, ours the int every caller keeps.
    struct PollFd
    {
        int fd;
        short events;
        short revents;
    };

    // Winsock, started once for the process (never stopped: the process's end does it).
    inline bool netStarted()
    {
        static const bool ok = [] {
            WSADATA data{};
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        return ok;
    }

    inline int asFd(SOCKET s) { return s == INVALID_SOCKET ? -1 : static_cast<int>(s); }
    inline SOCKET asSocket(int fd) { return static_cast<SOCKET>(static_cast<unsigned int>(fd)); }

    inline std::wstring wide(const std::string& utf8)
    {
        if (utf8.empty())
            return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring w(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
        return w;
    }

    inline std::string utf8(const std::wstring& w)
    {
        if (w.empty())
            return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    inline void closeFd(int fd) { closesocket(asSocket(fd)); }
    inline void removePath(const std::string& path) { DeleteFileW(wide(path).c_str()); }
    // `from` renamed over `to` in one step: a reader finds the old file or the new one, never none. One holding `to` open
    // without FILE_SHARE_DELETE (JUCE's readers share it; a scanner may not) refuses it for that moment: tried again for
    // up to half a second.
    inline bool replacePath(const std::string& from, const std::string& to)
    {
        const std::wstring f = wide(from), t = wide(to);
        for (int tries = 0; tries < 100; ++tries)
        {
            if (MoveFileExW(f.c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING) != 0)
                return true;
            const DWORD e = GetLastError();
            if (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED && e != ERROR_LOCK_VIOLATION)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    inline bool pathExists(const std::string& path) { return GetFileAttributesW(wide(path).c_str()) != INVALID_FILE_ATTRIBUTES; }
    inline long long recvSome(int fd, char* data, std::size_t n) { return recv(asSocket(fd), data, static_cast<int>(n), 0); }
    inline long long sendSome(int fd, const char* data, std::size_t n) { return send(asSocket(fd), data, static_cast<int>(n), 0); }
    inline bool interrupted() { return WSAGetLastError() == WSAEINTR; }
    inline bool wouldBlock()
    {
        const int e = WSAGetLastError();
        return e == WSAEWOULDBLOCK || e == WSAENOBUFS || e == WSAEINPROGRESS;
    }
    inline bool refused()
    {
        const int e = WSAGetLastError();
        return e == WSAECONNREFUSED || e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == WSAENETDOWN;
    }
    inline int processId() { return static_cast<int>(GetCurrentProcessId()); }
    // Whether process `pid` still runs (an instance's card names its process: one left by a crash is not listed).
    inline bool processAlive(int pid)
    {
        if (pid <= 0)
            return false;
        const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
        if (h == nullptr)
            return GetLastError() == ERROR_ACCESS_DENIED;
        DWORD code = 0;
        const bool alive = GetExitCodeProcess(h, &code) != 0 && code == STILL_ACTIVE;
        CloseHandle(h);
        return alive;
    }

    inline int pollFds(PollFd* fds, std::size_t n, int timeoutMs)
    {
        std::vector<WSAPOLLFD> w(n);
        for (std::size_t k = 0; k < n; ++k)
        {
            w[k].fd = asSocket(fds[k].fd);
            w[k].events = fds[k].events;
            w[k].revents = 0;
        }
        const int r = n == 0 ? (Sleep(static_cast<DWORD>(timeoutMs < 0 ? 0 : timeoutMs)), 0)
                             : WSAPoll(w.data(), static_cast<ULONG>(n), timeoutMs);
        for (std::size_t k = 0; k < n; ++k)
            fds[k].revents = w[k].revents;
        return r;
    }

    // Whether the other end of a non-blocking socket is still there (a peer gone reads as end of file).
    inline bool peerAlive(int fd)
    {
        PollFd p{ fd, POLLIN, 0 };
        if (pollFds(&p, 1, 0) < 0)
            return false;
        if ((p.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
            return false;
        if ((p.revents & POLLIN) != 0)
        {
            char c = 0;
            return recv(asSocket(fd), &c, 1, MSG_PEEK) != 0;
        }
        return true;
    }

    // Two connected sockets over the loopback (Winsock has no socketpair): what one sends, the other reads.
    inline bool socketPair(int out[2])
    {
        if (!netStarted())
            return false;
        const SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (l == INVALID_SOCKET)
            return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        int len = sizeof a;
        SOCKET c = INVALID_SOCKET, d = INVALID_SOCKET;
        if (bind(l, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0 && listen(l, 1) == 0
            && getsockname(l, reinterpret_cast<sockaddr*>(&a), &len) == 0)
        {
            c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (c != INVALID_SOCKET && connect(c, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0)
                d = accept(l, nullptr, nullptr);
        }
        closesocket(l);
        if (d == INVALID_SOCKET)
        {
            if (c != INVALID_SOCKET)
                closesocket(c);
            return false;
        }
        BOOL one = TRUE;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
        setsockopt(d, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
        out[0] = asFd(d);
        out[1] = asFd(c);
        return true;
    }

    // What `h` gives (a pipe, the console's stdin), as a socket the caller polls and reads: a thread of its own copies
    // it across and closes its end at the end of `h`'s data, which the reader sees as the end of the file. -1 when
    // no socket can be had. `closeHandle`: the thread closes `h` when it ends.
    inline int bridgeFrom(HANDLE h, bool closeHandle)
    {
        int pair[2];
        if (!socketPair(pair))
            return -1;
        std::thread([h, writer = pair[1], closeHandle] {
            char chunk[65536];
            for (;;)
            {
                DWORD got = 0;
                if (!ReadFile(h, chunk, sizeof chunk, &got, nullptr) || got == 0)
                    break;
                DWORD sent = 0;
                while (sent < got)
                {
                    const int n = send(asSocket(writer), chunk + sent, static_cast<int>(got - sent), 0);
                    if (n <= 0)
                        break;
                    sent += static_cast<DWORD>(n);
                }
                if (sent < got)
                    break;
            }
            closesocket(asSocket(writer));
            if (closeHandle)
                CloseHandle(h);
        }).detach();
        return pair[0];
    }

    // A socket the caller writes (non-blocking, with POLLOUT, as any) whose bytes a thread of its own writes to `h` (a
    // pipe), which it closes once the caller closes the socket, or `h`'s reader is gone. -1 when no socket can be had.
    inline int bridgeTo(HANDLE h)
    {
        int pair[2];
        if (!socketPair(pair))
            return -1;
        std::thread([h, reader = pair[1]] {
            char chunk[65536];
            for (;;)
            {
                const int n = recv(asSocket(reader), chunk, sizeof chunk, 0);
                if (n <= 0)
                    break;
                DWORD written = 0, at = 0;
                while (at < static_cast<DWORD>(n) && WriteFile(h, chunk + at, static_cast<DWORD>(n) - at, &written, nullptr))
                    at += written;
                if (at < static_cast<DWORD>(n))
                    break;
            }
            CloseHandle(h);
            closesocket(asSocket(reader));
        }).detach();
        return pair[0];
    }

    // A byte to the write end of a socketPair (the link's wake-up).
    inline long long pairWrite(int fd, const char* data, std::size_t n) { return sendSome(fd, data, n); }

    // stdin as a descriptor to poll and read: Windows bridges it to a socket once.
    inline int stdinFd()
    {
        static const int fd = [] {
            _setmode(0, _O_BINARY);
            return bridgeFrom(GetStdHandle(STD_INPUT_HANDLE), false);
        }();
        return fd;
    }

    // Bytes to a C runtime descriptor (stdout's, a reply's), in binary mode: no CR added before a line feed.
    inline long long writeFd(int fd, const char* data, std::size_t n)
    {
        return _write(fd, data, static_cast<unsigned int>(n));
    }
#else
    using PollFd = pollfd;

    inline bool netStarted() { return true; }
    inline void closeFd(int fd) { close(fd); }
    inline void removePath(const std::string& path) { unlink(path.c_str()); }
    // `from` renamed over `to` in one step (rename(2)): a reader finds the old file or the new one, never none.
    inline bool replacePath(const std::string& from, const std::string& to) { return std::rename(from.c_str(), to.c_str()) == 0; }
    inline bool pathExists(const std::string& path)
    {
        struct stat st{};
        return lstat(path.c_str(), &st) == 0;
    }
    inline long long recvSome(int fd, char* data, std::size_t n) { return read(fd, data, n); }   // a socket's or a pipe's
    inline long long sendSome(int fd, const char* data, std::size_t n)
    {
#if defined(MSG_NOSIGNAL)
        return send(fd, data, n, MSG_NOSIGNAL);
#else
        return send(fd, data, n, 0);
#endif
    }
    inline bool interrupted() { return errno == EINTR; }
    inline bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS; }
    inline bool refused() { return errno == ECONNREFUSED || errno == ENOENT; }
    inline int processId() { return static_cast<int>(getpid()); }
    inline bool processAlive(int pid) { return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM); }
    inline int pollFds(PollFd* fds, std::size_t n, int timeoutMs) { return poll(fds, static_cast<nfds_t>(n), timeoutMs); }

    inline bool peerAlive(int fd)
    {
        pollfd p{ fd, POLLIN, 0 };
        if (poll(&p, 1, 0) < 0)
            return false;
        if ((p.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
            return false;
        if ((p.revents & POLLIN) != 0)
        {
            char c = 0;
            return recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) != 0;
        }
        return true;
    }

    // A pipe: what one end gets written, the other reads (the link's wake-up).
    inline bool socketPair(int out[2]) { return pipe(out) == 0; }
    inline long long pairWrite(int fd, const char* data, std::size_t n) { return write(fd, data, n); }

    inline int stdinFd() { return 0; }
    inline long long writeFd(int fd, const char* data, std::size_t n) { return write(fd, data, n); }
#endif

    // The folder (made private when missing): "" when there is none to be had.
    inline std::string folder()
    {
        const std::string override = std::string(product::kEnvPrefix) + "MCP_DIR";
        std::string dir;
        if (const char* o = std::getenv(override.c_str()); o != nullptr && *o != '\0')
            dir = o;
        else
        {
#if defined(__APPLE__)
            char tmp[1024] = {};
            if (confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp) == 0)
                return {};
            dir = tmp;
#elif defined(_WIN32)
            wchar_t tmp[MAX_PATH + 1] = {};
            const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
            if (n == 0 || n > MAX_PATH)
                return {};
            dir = utf8(std::wstring(tmp, n));
            for (char& c : dir)
                if (c == '\\')
                    c = '/';
#else
            const char* run = std::getenv("XDG_RUNTIME_DIR");
            dir = run != nullptr && *run != '\0' ? std::string(run) : "/tmp/funk-" + std::to_string(getuid());
#endif
            if (!dir.empty() && dir.back() != '/')
                dir += '/';
            dir += "Funk-" + std::string(product::kName) + "-mcp";
        }
        while (dir.size() > 1 && dir.back() == '/')
            dir.pop_back();
#if defined(_WIN32)
        // In the user's own temp folder, which the profile keeps private: made when missing, a file there refused.
        const std::wstring w = wide(dir);
        const DWORD attrs = GetFileAttributesW(w.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            const std::size_t slash = dir.rfind('/');
            if (slash != std::string::npos && slash > 0)
                CreateDirectoryW(wide(dir.substr(0, slash)).c_str(), nullptr);
            if (!CreateDirectoryW(w.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                return {};
        }
        else if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
            return {};
        return dir;
#else
        struct stat st{};
        if (stat(dir.c_str(), &st) != 0)
        {
            // Its parent first, for an override that names a folder in a folder not yet made.
            const std::size_t slash = dir.rfind('/');
            if (slash != std::string::npos && slash > 0)
                mkdir(dir.substr(0, slash).c_str(), 0700);
            if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
                return {};
        }
        else if (!S_ISDIR(st.st_mode) || st.st_uid != getuid())
            return {};
        chmod(dir.c_str(), 0700);
        return dir;
#endif
    }

    inline bool fits(const std::string& path) { return path.size() < sizeof(sockaddr_un{}.sun_path); }

    inline void noSigpipe([[maybe_unused]] int fd)
    {
#if defined(SO_NOSIGPIPE)
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    }

    // A listening socket at `path` when no file is there (another instance's, or one left by an instance gone, which
    // claim() clears first), or -1.
    inline int listenFresh(const std::string& path)
    {
        if (!fits(path) || !netStarted())
            return -1;
#if defined(_WIN32)
        const int fd = asFd(socket(AF_UNIX, SOCK_STREAM, 0));
#else
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
#endif
        if (fd < 0)
            return -1;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
#if defined(_WIN32)
        if (bind(asSocket(fd), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 || listen(asSocket(fd), 8) != 0)
        {
            closeFd(fd);
            return -1;
        }
#else
        if (bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 || listen(fd, 8) != 0)
        {
            close(fd);
            return -1;
        }
        chmod(path.c_str(), 0600);
#endif
        return fd;
    }

    // A listening socket at `path` (one left there before is replaced), or -1.
    inline int listenAt(const std::string& path)
    {
        removePath(path);
        return listenFresh(path);
    }

    // A connection to the socket at `path`, or -1.
    // A connection to the socket at `path`, or -1 (refused() then says whether nothing listens there).
    inline int connectTo(const std::string& path)
    {
        if (!fits(path) || !netStarted())
            return -1;
#if defined(_WIN32)
        const int fd = asFd(socket(AF_UNIX, SOCK_STREAM, 0));
#else
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
#endif
        if (fd < 0)
            return -1;
        noSigpipe(fd);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
#if defined(_WIN32)
        if (connect(asSocket(fd), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0)
        {
            const int e = WSAGetLastError();
            closeFd(fd);
            WSASetLastError(e);                          // for refused()
            return -1;
        }
#else
        if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0)
        {
            const int e = errno;
            close(fd);
            errno = e;
            return -1;
        }
#endif
        return fd;
    }

    // A connection waiting on the listening socket `fd`, or -1 when none is.
    inline int acceptOn(int fd)
    {
#if defined(_WIN32)
        return asFd(accept(asSocket(fd), nullptr, nullptr));
#else
        return accept(fd, nullptr, nullptr);
#endif
    }

    // An instance's socket: the lowest number free in `dir` (a socket nothing listens on, left by an instance gone, is
    // cleared and taken). Its listening descriptor, its number and its path; -1 when none can be had.
    inline int claim(const std::string& dir, int& number, std::string& path)
    {
        for (int k = 1; k < 1000; ++k)
        {
            const std::string p = dir + "/" + std::to_string(k) + ".sock";
            if (pathExists(p))
            {
                if (const int fd = connectTo(p); fd >= 0)
                {
                    closeFd(fd);                             // an instance listens there
                    continue;
                }
                if (!refused())
                    continue;
                removePath(p);                               // left by an instance gone
            }
            if (const int fd = listenFresh(p); fd >= 0)
            {
                number = k;
                path = p;
                return fd;
            }
        }
        return -1;
    }

    // `fd` non-blocking: a read or a write that would wait returns at once (EAGAIN) instead.
    inline bool nonBlocking(int fd)
    {
#if defined(_WIN32)
        u_long on = 1;
        return ioctlsocket(asSocket(fd), static_cast<long>(FIONBIO), &on) == 0;
#else
        const int flags = fcntl(fd, F_GETFL, 0);
        return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    }

    // What of `pending` the non-blocking socket `fd` takes now, sent and removed from it; the rest waits for the socket
    // to have room (poll's POLLOUT). False when the other end is gone (`pending` is then dropped: nothing reaches it).
    inline bool flush(int fd, std::string& pending)
    {
        std::size_t sent = 0;
        bool open = true;
        while (sent < pending.size())
        {
            const long long n = sendSome(fd, pending.data() + sent, pending.size() - sent);
            if (n > 0)
            {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && interrupted())
                continue;
            if (n < 0 && wouldBlock())
                break;
            open = false;
            sent = pending.size();
        }
        pending.erase(0, sent);
        return open;
    }

    // `line` and a line feed, all of it, waiting while the socket is full (a blocking socket: the probes'); false when
    // the other end is gone.
    inline bool writeLine(int fd, const std::string& line)
    {
        const std::string out = line + "\n";
        std::size_t sent = 0;
        while (sent < out.size())
        {
            const long long n = sendSome(fd, out.data() + sent, out.size() - sent);
            if (n < 0 && interrupted())
                continue;
            if (n <= 0)
                return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    // Lines from a socket: what has come, a line at a time.
    class LineReader
    {
    public:
        explicit LineReader(int fd) : fd_(fd) {}

        // 1: a line in `out`; 0: none within timeoutMs (-1 waits); -1: the other end closed, or a line too long.
        int next(std::string& out, int timeoutMs)
        {
            for (;;)
            {
                if (const std::size_t nl = buffer_.find('\n'); nl != std::string::npos)
                {
                    out = buffer_.substr(0, nl);
                    buffer_.erase(0, nl + 1);
                    return 1;
                }
                if (buffer_.size() > kMaxLine)
                    return -1;
                PollFd p{ fd_, POLLIN, 0 };
                const int r = pollFds(&p, 1, timeoutMs);
                if (r < 0 && interrupted())
                    continue;
                if (r == 0)
                    return 0;
                if (r < 0)
                    return -1;
                if (!readSome())
                    return -1;
            }
        }

        // What is there to read now, without waiting: false when the other end closed.
        bool readSome()
        {
            char chunk[65536];
            const long long n = recvSome(fd_, chunk, sizeof chunk);
            if (n < 0 && (interrupted() || wouldBlock()))
                return true;
            if (n <= 0)
                return false;
            buffer_.append(chunk, static_cast<std::size_t>(n));
            return true;
        }

        // A whole line already read, without waiting.
        bool take(std::string& out)
        {
            const std::size_t nl = buffer_.find('\n');
            if (nl == std::string::npos)
                return false;
            out = buffer_.substr(0, nl);
            buffer_.erase(0, nl + 1);
            return true;
        }

        int fd() const noexcept { return fd_; }

    private:
        int fd_;
        std::string buffer_;
    };
}
