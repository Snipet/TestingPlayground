// Source/plugin/GitProjects.h: a project (ProjectStore.h) kept in a git repository of its own (package 05): the
// operations the git card's buttons start, over the store, the links file (ProjectStore.h's Link) and the user's own git
// (Git.h). Each blocks for as long as git runs, so it runs on the processor's git job (plugin/ProcessorGit.cpp), never on
// the audio or the message thread; git reaches the remote only here, only when the user pressed the button (Connect,
// Push, Pull, Check, Add from Git). Nothing is thrown: an outcome holds what happened in words, git's own when it
// failed.
//
// The working folder is <projects folder>/Git/<tidy name>-<8 hex of the project's id>/ (a number after it when a folder
// of that name is there already: never one that was there), a normal repository the user may also open elsewhere; its
// tree is ProjectTree.h's. Nothing is deleted that this call did not make: Connect and Add from Git remove their folder
// only when this very call made it and then failed (a refused remote, a clone that is no project); the Git folder that
// is a link, or a working folder that is one, is refused, never followed. A write of the tree removes only what the
// plugin's own last write of that project listed (Link::written).
//
//   connect  a commit a kept version, oldest first (its note the message, its save the date, a Playground-Version
//            trailer), the remote added, its branch looked at (one with commits refused: not empty), then pushed
//   push     a commit a version newer than the one last committed, then the push (never forced: a remote that moved
//            is "behind: pull first", its counts fetched)
//   pull     fetch, then nothing new, or a fast-forward: the remote's tree read from the fetched commit (nothing in the
//            folder changed when it reads as no project), the folder moved to it, and it saved as a new version (its
//            note "Pulled <sha7>: <subject>"); both sides moved: diverged (the next package settles it)
//   check    fetch, and the counts
//   add      a clone into a new folder, its tree added as a new project (a number after a name taken), linked
//   disconnect  the link removed; the folder stays
#pragma once

#include "plugin/Git.h"
#include "plugin/ProjectStore.h"

#include <atomic>
#include <functional>
#include <string>

namespace playground::gitprojects
{
    struct Env
    {
        std::string git;                                  // git::Found::path
        const std::atomic<bool>* cancel = nullptr;
        bool allowFile = false;                           // file:// remotes (the probes')
        std::function<void(const std::string&)> doing;    // what it does now, in words ("Pushing to github.com/me/pad...")
        int networkMs = git::kNetworkMs, localMs = git::kLocalMs;
    };

    struct Outcome
    {
        bool ok = false;
        bool cancelled = false;
        std::string words;                                // what happened ("Pushed 3 versions to github.com/me/pad."),
                                                          // or why not (git's own last lines, with advice)
        std::string project;                              // add: the new project's name
        int version = 0;                                  // pull: the version it saved (0: none)
        bool behind = false;                              // push: refused, the remote moved
        bool diverged = false;                            // pull: both sides have commits
    };

    // The remote's address as the card shows it: "github.com/me/pad" for https://github.com/me/pad.git and
    // git@github.com:me/pad.git.
    std::string shortRemote(const std::string& remote);

    // The working folder of a link (none: "").
    std::string folderOf(const projectstore::Link& link);

    // Versions of the project newer than the link's last commit (what Push would commit), from the store's cache when
    // `versions` (oldest first) are given.
    int toPush(const projectstore::Link& link, const std::vector<int>& versions);

    Outcome connect(const Env& env, const std::string& project, const std::string& remote, const std::string& branch);
    Outcome push(const Env& env, const std::string& project);
    Outcome pull(const Env& env, const std::string& project);
    Outcome check(const Env& env, const std::string& project);
    Outcome add(const Env& env, const std::string& remote, const std::string& branch);
    Outcome disconnect(const std::string& project);

    // Words added to git's own for what a sandboxed host, an SSH key with a passphrase or a missing identity need.
    std::string advice(const std::string& why, bool notStarted);
}
