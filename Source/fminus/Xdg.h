// Source/fminus/Xdg.h: the user's folders on Linux, as the XDG Base Directory specification names them, for every side
// that keeps a file of the user's there: the plugin's projects and chats (plugin/ProjectStore.cpp, ChatStore.cpp:
// $XDG_DATA_HOME), the shaders' disk cache (fminus/Shaders.cpp: $XDG_CACHE_HOME), and what lives only while the user is
// logged in, the MCP sockets (mcp/McpProtocol.h) and a shader build's temporary folder ($XDG_RUNTIME_DIR). Plain C++
// and POSIX, here in the lowest layer so that the plugin, the MCP server, the GUI sandbox's helper and the renderer can
// all include it; macOS and Windows never call it.
//
// The rules (the specification's): a variable that holds a relative path is ignored, as if it were unset, and the
// default under the home is taken ($XDG_DATA_HOME: ~/.local/share, $XDG_CONFIG_HOME: ~/.config, $XDG_CACHE_HOME:
// ~/.cache). $XDG_RUNTIME_DIR has no default: it is taken only when it is a folder (not a link) of this user's that no
// one else may enter (no group or other bits), else there is none and the caller says what it does instead. The home is
// $HOME, else the password database's.
#pragma once

#if !defined(_WIN32)
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace playground::xdg
{
    // The user's home: $HOME when it is an absolute path, else the password database's; "" when neither.
    inline std::string home()
    {
        if (const char* h = std::getenv("HOME"); h != nullptr && h[0] == '/')
            return h;
        std::vector<char> buf(16384);
        passwd pw{};
        passwd* found = nullptr;
        if (getpwuid_r(getuid(), &pw, buf.data(), buf.size(), &found) == 0 && found != nullptr && pw.pw_dir != nullptr
            && pw.pw_dir[0] == '/')
            return pw.pw_dir;
        return {};
    }

    // $<var> when it is an absolute path, else <home>/<fallback>; "" when there is neither.
    inline std::string base(const char* var, const char* fallback)
    {
        if (const char* v = std::getenv(var); v != nullptr && v[0] == '/')
        {
            std::string s = v;
            while (s.size() > 1 && s.back() == '/')
                s.pop_back();
            return s;
        }
        const std::string h = home();
        if (h.empty())
            return {};
        return (h.size() > 1 && h.back() == '/' ? h : h + "/") + fallback;
    }

    inline std::string dataHome() { return base("XDG_DATA_HOME", ".local/share"); }
    inline std::string configHome() { return base("XDG_CONFIG_HOME", ".config"); }
    inline std::string cacheHome() { return base("XDG_CACHE_HOME", ".cache"); }

    // Whether `dir` is a folder of this user's (not a link) that no one else may write into (`strict`: nor enter: no
    // group or other bits at all).
    inline bool ownPrivateFolder(const std::string& dir, bool strict)
    {
        struct stat st{};
        if (dir.empty() || lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid())
            return false;
        return (st.st_mode & (strict ? mode_t{ 077 } : mode_t{ 022 })) == 0;
    }

    // $XDG_RUNTIME_DIR when it is an absolute path to a folder of this user's that no one else may enter; else "".
    inline std::string runtimeDir()
    {
        const char* v = std::getenv("XDG_RUNTIME_DIR");
        if (v == nullptr || v[0] != '/')
            return {};
        std::string s = v;
        while (s.size() > 1 && s.back() == '/')
            s.pop_back();
        return ownPrivateFolder(s, true) ? s : std::string();
    }
}
#endif
