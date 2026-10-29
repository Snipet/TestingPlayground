// Source/installer/System.cpp: the engine's system tools on this Mac (Engine.h's MacSystem): ditto, pgrep, killall,
// NSFileManager's trash (through juce::File::moveToTrash) and the administrator prompt (osascript). Every program runs
// without a shell, its arguments as they are; the one shell is the administrator's /bin/sh running the engine's script,
// whose path osascript gets as an argument (never spliced into AppleScript's text).
#include "installer/Engine.h"

#include <juce_core/juce_core.h>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace playground::installer
{
    int runProgram(const std::vector<std::string>& argv, std::string* out, std::string* err)
    {
        if (argv.empty())
            return -1;
        int outPipe[2] = { -1, -1 }, errPipe[2] = { -1, -1 };
        if (::pipe(outPipe) != 0)
            return -1;
        if (::pipe(errPipe) != 0)
        {
            ::close(outPipe[0]);
            ::close(outPipe[1]);
            return -1;
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, outPipe[1], 1);
        posix_spawn_file_actions_adddup2(&actions, errPipe[1], 2);
        posix_spawn_file_actions_addclose(&actions, outPipe[0]);
        posix_spawn_file_actions_addclose(&actions, errPipe[0]);
        std::vector<char*> args;
        for (const std::string& a : argv)
            args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        pid_t pid = 0;
        const int spawned = ::posix_spawn(&pid, args[0], &actions, nullptr, args.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(outPipe[1]);
        ::close(errPipe[1]);
        if (spawned != 0)
        {
            ::close(outPipe[0]);
            ::close(errPipe[0]);
            if (err != nullptr)
                *err = std::string("could not start ") + argv[0] + " (" + std::strerror(spawned) + ")";
            return -1;
        }
        // Both pipes read until they close, so that neither fills and stops the program.
        pollfd fds[2] = { { outPipe[0], POLLIN, 0 }, { errPipe[0], POLLIN, 0 } };
        int open = 2;
        char buf[4096];
        while (open > 0)
        {
            if (::poll(fds, 2, -1) < 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }
            for (int k = 0; k < 2; ++k)
            {
                if (fds[k].fd < 0 || (fds[k].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
                    continue;
                const ssize_t n = ::read(fds[k].fd, buf, sizeof buf);
                if (n > 0)
                {
                    std::string* to = k == 0 ? out : err;
                    if (to != nullptr)
                        to->append(buf, static_cast<std::size_t>(n));
                }
                else if (n == 0 || errno != EINTR)
                {
                    ::close(fds[k].fd);
                    fds[k].fd = -1;
                    --open;
                }
            }
        }
        for (const pollfd& f : fds)
            if (f.fd >= 0)
                ::close(f.fd);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0)
            if (errno != EINTR)
                return -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    bool dittoCopy(const fs::path& from, const fs::path& to, std::string& error)
    {
        std::string err;
        const int status = runProgram({ "/usr/bin/ditto", "--noqtn", from.string(), to.string() }, nullptr, &err);
        if (status == 0)
            return true;
        while (!err.empty() && (err.back() == '\n' || err.back() == ' '))
            err.pop_back();
        error = !err.empty() ? err : "ditto stopped with status " + std::to_string(status);
        return false;
    }

    bool MacSystem::copy(const fs::path& from, const fs::path& to, std::string& error)
    {
        return dittoCopy(from, to, error);
    }

    std::vector<fs::path> MacSystem::runningApps()
    {
        // Every process whose executable lies in an app bundle: "<pid> <command line>", the bundle up to its .app.
        std::string out;
        runProgram({ "/usr/bin/pgrep", "-lf", "\\.app/Contents/MacOS/" }, &out, nullptr);
        std::vector<fs::path> apps;
        std::size_t at = 0;
        while (at < out.size())
        {
            std::size_t end = out.find('\n', at);
            if (end == std::string::npos)
                end = out.size();
            const std::string line = out.substr(at, end - at);
            at = end + 1;
            const std::size_t space = line.find(' ');
            const std::size_t app = line.find(".app/Contents/MacOS/");
            if (space == std::string::npos || app == std::string::npos || app < space)
                continue;
            apps.emplace_back(line.substr(space + 1, app + 4 - (space + 1)));
        }
        return apps;
    }

    bool MacSystem::restartAuRegistrar()
    {
        // 0: stopped (launchd starts it again when a host asks); 1: none was running, which is as good.
        const int status = runProgram({ "/usr/bin/killall", "-9", "AudioComponentRegistrar" }, nullptr, nullptr);
        return status == 0 || status == 1;
    }

    bool MacSystem::moveToTrash(const fs::path& path, std::string& error)
    {
        if (juce::File(juce::String::fromUTF8(path.c_str())).moveToTrash())
            return true;
        error = "macOS would not move it to the Trash";
        return false;
    }

    System::Admin MacSystem::runAsAdministrator(const fs::path& script, const std::string& prompt, std::string& error)
    {
        // The script's path and the prompt reach AppleScript as arguments (argv), so nothing is quoted into its text.
        std::string out, err;
        const int status = runProgram({ "/usr/bin/osascript",
                                        "-e", "on run argv",
                                        "-e", "do shell script \"/bin/sh \" & quoted form of (item 1 of argv) "
                                              "with prompt (item 2 of argv) with administrator privileges",
                                        "-e", "end run",
                                        script.string(), prompt },
                                      &out, &err);
        if (status == 0)
            return Admin::ran;
        if (err.find("-128") != std::string::npos)        // "User canceled. (-128)"
            return Admin::cancelled;
        error = err.empty() ? "osascript stopped with status " + std::to_string(status) : err;
        return Admin::failed;
    }
}
