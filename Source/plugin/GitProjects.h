// Source/plugin/GitProjects.h: a project kept in a git repository of the user's (docs/plan/05-git.md; CLAUDE.md,
// "Running git (the user's)"). The operations the Projects card's Git... offers, each over the store (ProjectStore.h,
// its links file) and the user's own git (plugin/Git.h), on the caller's thread, which is never the audio or the message
// thread (the Processor's git job: plugin/ProcessorGit.cpp; the probes):
//
//   connect     a project given a remote and a branch (main unless said): its working folder made
//               (<projects folder>/Git/<name>-<8 hex of its id>/), `git init`, a commit for each kept version from the
//               oldest (its tree, ProjectExport.h's writeTree; "Version N: <note>", its save's time, the trailer
//               Playground-Version: N), the remote added, and pushed (-u). A remote whose branch has commits already is
//               refused before anything is committed, and the folder made for it taken away again (it held nothing else)
//   push        a commit for each version newer than the last committed, then `git push`: never forced; a remote that
//               moved refuses it ("behind": pull first), and nothing else changes
//   pull        `git fetch`, then nothing new; a fast-forward (no version here since the last commit): the tree read and
//               saved as a new version ("Pulled <sha7>: <subject>", "and N more commits"), with its presets and info;
//               or diverged (both sides have changes): nothing changed, said
//   check       `git fetch` and the counts: how far ahead and behind
//   clone       Add from Git: `git clone` into a new folder, its tree read and added as a new project (projectstore::add:
//               "<name> 2" when the name is taken), linked. The other kind's project, or a repository that holds none, is
//               refused and the clone taken away again
//   disconnect  the link removed; the working folder stays where it is, and the outcome says where
//
// Each returns a Result: ok, or why in words (git's own last lines as they are when git said something, with a line on
// what a sandboxed host or an SSH key with a passphrase needs when that is what it looks like). Nothing is ever forced,
// and nothing of the user's is deleted: a version removed in the store stays a commit in the repository, and the only
// folders taken away are the ones the same call made and filled with nothing but its own work.
#pragma once

#include "plugin/Git.h"
#include "plugin/ProjectStore.h"

#include <atomic>
#include <functional>
#include <string>

namespace juce
{
    class File;
}

namespace playground::gitprojects
{
    inline constexpr const char* kGitFolder = "Git";              // in the projects folder
    inline constexpr const char* kTrailer = "Playground-Version";   // a commit's version number

    struct Setup
    {
        std::string git;                                    // git's path (git::find)
        const std::atomic<bool>* cancel = nullptr;
        bool allowFile = false;                             // file:// remotes (the probes')
        std::function<void(const std::string&)> step;       // what it does now, in words ("Committing version 3 of 5")
    };

    struct Result
    {
        bool ok = false;
        std::string why;                    // !ok: in words
        std::string line;                   // ok: what was done, in words ("Pushed 3 versions to github.com/me/pad.")
        std::string project;                // the project acted on (a clone's: the name it took)
        int version = 0;                    // a pull's or a clone's version saved (0: none)
        bool nothing = false;               // a pull with nothing new
        bool behind = false;                // a push refused: the remote moved (pull first)
        bool diverged = false;              // a pull refused: both sides changed
        bool cancelled = false;
    };

    Result connect(const Setup& s, const std::string& project, const std::string& remote, const std::string& branch);
    Result push(const Setup& s, const std::string& project);
    Result pull(const Setup& s, const std::string& project);
    Result check(const Setup& s, const std::string& project);
    Result clone(const Setup& s, const std::string& remote, const std::string& branch);
    Result disconnect(const std::string& project);

    // The folder a project's repository is made in: <projects folder>/Git/<name>-<8 hex of its id>, a number after it
    // when that is taken.
    juce::File folderFor(const std::string& project, const std::string& id);
    // A remote's address shortened for the card and the mark's title: "github.com/me/pad" for
    // https://github.com/me/pad.git, git@github.com:me/pad.git and ssh://git@github.com/me/pad.
    std::string shortRemote(const std::string& address);
    // A commit's message for version `number` with its note.
    std::string messageOf(int number, const std::string& note);
}
