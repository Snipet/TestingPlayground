// Source/plugin/FileReplace.h: a file written beside its place renamed over it in one step, on every platform, the
// way every store of the plugin's that another process reads keeps its file (Usage.cpp's usage.json, FactoryStore.cpp's
// factory.json and its set, UpdateCheck.cpp's app.json, ChatStore.cpp's key and chats), and the pair that makes a path
// of UTF-8 text and back (pathOf, utf8Of: the git code's). JUCE-free, so any of their sides may take it.
//
// POSIX: rename(2), which lands whatever reads the old file (a reader keeps the file it opened). Windows: MoveFileExW
// with MOVEFILE_REPLACE_EXISTING, which fails while another handle holds the old file without sharing its deletion, as
// every std::ifstream (the C runtime's _SH_DENYNO) and JUCE's FileInputStream do: the other product's process reading
// the same file at that moment. So it is tried again, every kRetryMs for up to kWaitMs, while the error is one a reader
// causes (ERROR_SHARING_VIOLATION, ERROR_ACCESS_DENIED, ERROR_LOCK_VIOLATION), and fails at once on any other. On a
// failure nothing moved: `from` is where it was (its caller removes it) and `to` is as it was. mcp/McpProtocol.h's
// replacePath is the same rule for the MCP server, which takes no plugin code.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace playground::files
{
    inline constexpr int kRetryMs = 5;
    inline constexpr int kWaitMs = 500;                  // how long a reader may hold `to` before the rename fails

    // `from` renamed over `to`; false with `ec` set when it did not move.
    bool replace(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec);

    // The plugin's paths are UTF-8 text (JUCE's toStdString, run::run's arguments and working folder), while
    // std::filesystem::path made from a std::string, and its string(), go through the process's ANSI code page on
    // Windows: a folder under a name outside it (Zoë, a Cyrillic or CJK account) would be another folder, or throw.
    // So a path is made from UTF-8 text by pathOf and given back as UTF-8 text by utf8Of, never fs::path(std::string)
    // nor path::string(). On POSIX both are the bytes as they are.
    inline std::filesystem::path pathOf(std::string_view utf8)
    {
        return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    }
    inline std::string utf8Of(const std::filesystem::path& p)
    {
        const std::u8string u = p.u8string();
        return std::string(u.begin(), u.end());
    }
}
