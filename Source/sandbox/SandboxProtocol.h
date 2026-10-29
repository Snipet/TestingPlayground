// Source/sandbox/SandboxProtocol.h: the GUI sandbox's process and its line protocol, between the MCP server
// (mcp/Main.cpp, its gui_sandbox tool) and the helper, <Name>GuiSandbox (Source/sandbox/Main.cpp). std only: both
// include it.
//
//   the process   the server starts the helper on its first gui_sandbox call (beside its own executable, or where
//                 <ENV_PREFIX>GUI_SANDBOX says), on two pipes: its stdin and its stdout; its stderr is the server's
//                 (Claude Desktop's MCP log). The helper writes nothing to stdout but replies (it moves stray output to
//                 stderr before it starts) and quits when its stdin closes: the server closes it after kIdleSeconds with
//                 no request (it owns the idle time, so that no request can cross the helper's quitting), and the
//                 helper quits by itself after kHelperIdleSeconds, should the server never do it
//   a request     one JSON object a line, one at a time (the server sends the next once this one is answered, or past
//                 its deadline, when it kills the helper): { "id": 1, "op": "...", ... }. Any request may carry
//                 load (a state, as the server kept it: the sandbox to make first, in a helper started again or for
//                 another sandbox's number; the reply then carries state, whatever came of the request) and expect
//                 (a sandbox's number: refused when another is open)
//                   open    from: blank | text | file | files (the server's: an instance's), with text (the .gui),
//                           path (a .gui file or a program's folder), files ([{ name, text }]), dials, origin (words),
//                           detail -> the sandbox's summary and its layout report
//                   edit    text (the .gui whole) | edits ([{ old_text, new_text }], each exactly once, all or none),
//                           dials, detail -> GuiSpec's notes by line and the layout report
//                   render  width, height (the area, logical px), theme (graphite | paper; else the user's), page,
//                           values ({ dial: value }), cap (the PNG's bytes at most; 0: whole) -> png (base64), pixels,
//                           area (the area now, which the server keeps in its state), text (a line)
//                   state   -> the sandbox, for the server to keep: what open needs to make it again in a helper
//                           started again (files, dials, origin, number, area)
//                   quit    -> {} and the helper ends
//   a reply       one line: { "id": 1, "ok": true, "text": "...", ... } or { "id": 1, "ok": false, "text": "why" };
//                 open's and edit's carry `state` too (the server keeps the newest)
#pragma once

#include "Product.h"

#include <cstddef>
#include <string>

namespace playground::sandbox
{
    inline constexpr int kIdleSeconds = 600;                 // the server closes the helper's stdin after this long
                                                             // with no request (<ENV_PREFIX>SANDBOX_IDLE_MS, the probes')
    inline constexpr int kHelperIdleSeconds = 720;           // the helper's own, should the server not have closed it
    inline constexpr int kDeadlineMs = 30000;                // a request's, the server's (<ENV_PREFIX>SANDBOX_DEADLINE_MS)

    // The area render draws (logical px) unless asked otherwise: the plugin's PANEL view at the editor's default size
    // (1360 x 760, the chat open), as view_panel's picture shows it; and its bounds.
    inline constexpr int kDefaultWidth = 853, kDefaultHeight = 612;
    inline constexpr int kMinSide = 160, kMaxSide = 2400;
    inline constexpr float kDpi = 1.5f;                      // the picture's pixels a logical px (view_panel's)

    // What open reads from disk: a .gui file, or a program's folder (its .fm, .gui and .sc files), read only.
    inline constexpr std::size_t kMaxFileBytes = 256u * 1024u;
    inline constexpr std::size_t kMaxFiles = 12;             // the editor's tabs (kMaxFiles)
    inline constexpr std::size_t kMaxTotalBytes = 1024u * 1024u;

    // The helper's file name, beside the server's.
    inline std::string helperName() { return std::string(product::kName) + "GuiSandbox"; }
}
