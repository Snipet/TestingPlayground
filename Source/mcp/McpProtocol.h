// Source/mcp/McpProtocol.h: how this playground's instances (plugin/McpLink.h) and its MCP server (mcp/Main.cpp, the
// program Claude Desktop starts) find and talk to each other.
//
//   the folder   one per user and plugin, private (0700): <the user's temp folder>/Funk-<plugin>-mcp/, or
//                <ENV_PREFIX>MCP_DIR when set (the probes' own). In it each instance's socket, <n>.sock: the lowest
//                number free when it started, its name to the user ("Synth 2", shown in the plugin while Claude Desktop
//                talks to it); and what the newest instance wrote for a server that finds none running: tools.json and
//                guide.md
//   a request    one JSON object a line: { "id": 1, "method": "info" | "tools" | "guide" | "call" | "cancel",
//                "params": { ... } }; several may be in flight on one connection
//   a reply      one line: { "id": 1, "result": ... } or { "id": 1, "error": "why" }, exactly one a request, in the
//                order they are ready (a quick one may pass a long call's)
//   never stuck  both ends' sockets are non-blocking, and each end queues what it writes and keeps reading while it
//                waits to send the rest: a large request and a large reply crossing each other (more than a socket's
//                buffer, 8 KB on macOS) never leave both ends waiting for the other to read
//                  info    the instance: { id, name, kind, plugin, host, track, project, version, preset, edited,
//                          files, compiled, busy }
//                  tools   the tools, as MCP lists them ({ name, title, description, inputSchema, annotations })
//                  guide   { part } -> a part of what the plugin's own chat works from, for an assistant that drives it
//                          from outside (mcp/McpGuide.h; "start" when no part is named; an error for one that is not)
//                  call    { name, arguments } -> { content: [ MCP content ], isError }: one at a time, in order, but
//                          for the live reads (ChatAgent::mcpLive), which answer at once
//                  cancel  { id } -> { cancelled }: stop the call request `id` made on this connection (one waiting
//                          is answered "stopped before it started"; the one running stops as soon as it can, and
//                          still answers)
// POSIX: Unix-domain sockets, no JUCE, so the server stays small.
#pragma once

#include "Product.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace playground::mcp
{
    inline constexpr std::size_t kMaxLine = 64u * 1024u * 1024u;   // a reply with pictures stays well under

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
    }

    inline bool fits(const std::string& path) { return path.size() < sizeof(sockaddr_un{}.sun_path); }

    inline void noSigpipe([[maybe_unused]] int fd)
    {
#if defined(SO_NOSIGPIPE)
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    }

    // A listening socket at `path` (one left there before is replaced), or -1.
    inline int listenAt(const std::string& path)
    {
        if (!fits(path))
            return -1;
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
        unlink(path.c_str());
        if (bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 || listen(fd, 8) != 0)
        {
            close(fd);
            return -1;
        }
        chmod(path.c_str(), 0600);
        return fd;
    }

    // A listening socket at `path` when no file is there (another instance's, or one left by an instance gone, which
    // claim() clears first), or -1.
    inline int listenFresh(const std::string& path)
    {
        if (!fits(path))
            return -1;
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
        if (bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 || listen(fd, 8) != 0)
        {
            close(fd);
            return -1;
        }
        chmod(path.c_str(), 0600);
        return fd;
    }

    // A connection to the socket at `path`, or -1.
    inline int connectTo(const std::string& path)
    {
        if (!fits(path))
            return -1;
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        noSigpipe(fd);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
        if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0)
        {
            close(fd);
            return -1;
        }
        return fd;
    }

    // An instance's socket: the lowest number free in `dir` (a socket nothing listens on, left by an instance gone, is
    // cleared and taken). Its listening descriptor, its number and its path; -1 when none can be had.
    inline int claim(const std::string& dir, int& number, std::string& path)
    {
        for (int k = 1; k < 1000; ++k)
        {
            const std::string p = dir + "/" + std::to_string(k) + ".sock";
            struct stat st{};
            if (lstat(p.c_str(), &st) == 0)
            {
                if (const int fd = connectTo(p); fd >= 0)
                {
                    close(fd);                               // an instance listens there
                    continue;
                }
                if (errno != ECONNREFUSED && errno != ENOENT)
                    continue;
                unlink(p.c_str());                           // left by an instance gone
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
        const int flags = fcntl(fd, F_GETFL, 0);
        return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    // What of `pending` the non-blocking socket `fd` takes now, sent and removed from it; the rest waits for the socket
    // to have room (poll's POLLOUT). False when the other end is gone (`pending` is then dropped: nothing reaches it).
    inline bool flush(int fd, std::string& pending)
    {
        std::size_t sent = 0;
        bool open = true;
        while (sent < pending.size())
        {
#if defined(MSG_NOSIGNAL)
            const ssize_t n = send(fd, pending.data() + sent, pending.size() - sent, MSG_NOSIGNAL);
#else
            const ssize_t n = send(fd, pending.data() + sent, pending.size() - sent, 0);
#endif
            if (n > 0)
            {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS))
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
#if defined(MSG_NOSIGNAL)
            const ssize_t n = send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
#else
            const ssize_t n = send(fd, out.data() + sent, out.size() - sent, 0);
#endif
            if (n < 0 && errno == EINTR)
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
                pollfd p{ fd_, POLLIN, 0 };
                const int r = poll(&p, 1, timeoutMs);
                if (r < 0 && errno == EINTR)
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
            const ssize_t n = read(fd_, chunk, sizeof chunk);   // a socket's or a pipe's
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
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
