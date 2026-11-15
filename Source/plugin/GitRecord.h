// Source/plugin/GitRecord.h: the git this computer runs for the user, remembered (package 05): the git the user chose
// in Settings, and what the last look found (a look of Settings' Look for git, or the one every git job makes before it
// runs: plugin/ProcessorGit.cpp), kept in git.json in the folder both products share (factorystore::directory(): the
// chat's key, factory.json and app.json are there), so that both the effect and the synth, every instance and every
// later launch show it at once, with no git run to show it. Git never runs by itself: nothing here runs git; a record is
// only what a run of the user's press found, brought up to date by the next one.
//
// The file: {"format": "funk-playground-git-machine", "version": 1, "chosen": "<path>" (a choice made: "" for none,
// find it as plugin/Git.h finds it), "found": {"path", "version", "ms"} (the last look found git), "none": {"why",
// "ms"} (the last look found none: why, in words)}. Every write is made under the folder's .lock (UpdateCheck's,
// FactoryStore's and the usage ledger's too), the file read again first, only the fields this write changes set and
// every other kept, then written beside its place and renamed in (files::replace). Read back by its size, time and
// identity, so a write of another process (the other product) is seen at the next look. A probe moves the folder with
// factorystore::setDirectoryForTests (under CTest it is the probe's sandbox's factory/ already).
//
// Before this record, a product kept its chosen git in its own projects folder's .git-links.json ("git"). That field is
// taken over: while the record has no "chosen" of its own, this product's old field is the choice; the first write of
// the record writes it there, and then the old field is removed from the links file, so the two never disagree (an
// older build then finds git itself). A product whose record already has a choice drops its old field unread.
#pragma once

#include "plugin/Git.h"

#include <cstdint>
#include <string>

namespace playground::gitrecord
{
    inline constexpr const char* kFile = "git.json";
    inline constexpr const char* kFormat = "funk-playground-git-machine";

    struct Record
    {
        std::string chosen;                      // the git the user chose ("": none, found as Git.h finds it)
        std::string path, version;               // what the last look found ("" none found, or never looked)
        int64_t foundMs = 0;                     // ... when (ms since 1970)
        std::string why;                         // the last look found none: why, in words ("" otherwise)
        bool looked() const noexcept { return !path.empty() || !why.empty(); }
    };

    // The record as it is (the file read again only when it changed; the old field taken over while it has no choice).
    Record read();
    // Moves when the record changes, here or in another process (a look of the file's size, time and identity).
    uint64_t revision();

    // The user's choice: set (a path, or "" to forget it), and what a look found under the choice before forgotten (the
    // look that follows a choice fills it again). False, with why, when it cannot be written.
    bool setChosen(const std::string& path, std::string* why = nullptr);
    // What a run's look found (git::find, with the choice in force then): written when it differs from the record. When
    // none was found and the record remembered one found before (with no choice made), `found.why` comes back in the
    // words that say so (the git found on this computer before, at <path>, is no longer there), and those are kept.
    void noteFound(git::Found& found, int64_t nowMs);
}
