// Source/installer/EnginePlatform.h: what the engine's shared half (Engine.cpp) asks of its platform's (EngineMac.cpp,
// EngineWin.cpp): what is at a path (never through a link), whether this user may write or move it, renames, the
// staging folder, the code signature, the clock, and the running apps and hosts by name; with the few words that
// differ. Private to the engine: Engine.h is the API.
#pragma once

#include "installer/Engine.h"

#include <string>

namespace playground::installer::platform
{
    // What is at a path, looked at without following a link there.
    struct Node
    {
        bool exists = false;                         // something is there (a link too, even one that leads nowhere)
        bool missing = false;                        // nothing is there (not: it could not be looked at)
        bool link = false;                           // a symbolic link (Windows: or a junction); then neither below
        bool dir = false;                            // a real folder (a bundle is one)
        bool file = false;                           // a regular file
        bool mine = true;                            // this user owns it
        std::string owner;                           // who does, in words, when this user does not
        int error = 0;                               // why it could not be looked at (errno; a Windows error code)
    };

    Node nodeOf(const fs::path& p);
    bool writable(const fs::path& dir);              // this user may add to this folder
    bool mayMove(const fs::path& path);              // this user may rename it away (aside, or to the Trash)
    std::string linkTarget(const fs::path& link);    // where a link leads, in words
    std::string errorText(int error);                // a Node's (or a rename's) error, in words

    // A rename to where nothing is (Windows) or that replaces it (macOS's rename(2): the engine renames only to free
    // names), one that never replaces (fails when something is there), and two items exchanged: macOS's RENAME_SWAP,
    // at once; Windows' three renames (b aside, a to b, b's old self to a; a failure on the way puts b back), so that
    // b is missing for a moment, never broken.
    bool renameItem(const fs::path& from, const fs::path& to, int& error);
    bool renameNew(const fs::path& from, const fs::path& to, int& error);
    bool swapItems(const fs::path& a, const fs::path& b, int& error);
    bool removeEmptyFolder(const fs::path& p);       // rmdir: an empty folder only (true when it went)
    // A new staging folder in `folder`, .installing-XXXXXX (six letters and digits, a name not taken); empty when it
    // could not be made (`error` says why).
    fs::path makeStage(const fs::path& folder, int& error);

    std::string cdhash(const fs::path& p);           // the code signature's CDHash in hex; "" when none (Windows)
    std::string now();                               // the local time, "YYYY-MM-DD hh:mm:ss"

    std::string appFile(const std::string& name);    // a Standalone's file as it runs: <name>.app, <name>.exe
    bool sameName(const fs::path& a, const fs::path& b);   // two file names, as the disk tells them apart
    std::string hostName(const fs::path& app);       // a running app that is a known host: its name; "" when not

    // Shared helpers (Engine.cpp).
    bool plainName(const std::string& s);            // one plain path component the payload may give
    std::string fileText(const fs::path& p);
    std::string trimmed(std::string s);
    const char* actionWords(Action a);

    // The words that differ.
#if defined(_WIN32)
    inline constexpr const char* kFolderKind = "a folder";            // what an app or a VST3 is, where it is not
    inline constexpr const char* kNoAdmin = "no administrator approved it";
    inline constexpr const char* kOpenWhileRemoved = " is open: quit it first, or Windows may not let it go to the "
                                                     "Recycle Bin.";
#else
    inline constexpr const char* kFolderKind = "a bundle";
    inline constexpr const char* kNoAdmin = "the administrator's password was not given";
    inline constexpr const char* kOpenWhileRemoved = " is open: it runs until you quit it, from the Trash.";
#endif
}
