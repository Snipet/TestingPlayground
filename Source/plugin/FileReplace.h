// Source/plugin/FileReplace.h: a file written beside its place renamed over it in one step, on every platform, the
// way every store of the plugin's that another process reads keeps its file (Usage.cpp's usage.json, FactoryStore.cpp's
// factory.json and its set, UpdateCheck.cpp's app.json, ChatStore.cpp's key and chats). JUCE-free, so any of their
// sides may take it.
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
#include <system_error>

namespace playground::files
{
    inline constexpr int kRetryMs = 5;
    inline constexpr int kWaitMs = 500;                  // how long a reader may hold `to` before the rename fails

    // `from` renamed over `to`; false with `ec` set when it did not move.
    bool replace(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec);
}
