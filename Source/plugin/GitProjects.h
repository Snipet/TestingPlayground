// Source/plugin/GitProjects.h: a project kept in a git repository of the user's (docs/plan/05-git.md; CLAUDE.md,
// "Running git (the user's)"). The operations the Projects card's Git... offers, each over the store (ProjectStore.h,
// its links file) and the user's own git (plugin/Git.h), on the caller's thread, which is never the audio or the message
// thread (the Processor's git job: plugin/ProcessorGit.cpp; the probes):
//
//   connect     a project given a remote and a branch (main unless said): its working folder made
//               (<projects folder>/Git/<name>-<8 hex of its id>/), `git init`, a commit for each kept version from the
//               oldest (its tree, ProjectExport.h's writeTree; "Version N: <note>", its save's time, the trailer
//               Playground-Version: N), the remote added, and pushed (-u). The remote is asked first, by its address and
//               with no repository: one whose branch has commits already (or that cannot be reached) is refused before
//               any folder is made. The folder is always a new one; a later failure leaves it, the words saying where
//   push        a commit for each version newer than the last committed, then `git push`: never forced; a remote that
//               moved refuses it ("behind": pull first), and nothing else changes
//   pull        `git fetch`, then nothing new; a fast-forward (no version here since the last commit): the tree read and
//               saved as a new version ("Pulled <sha7>: <subject>", "and N more commits"), with its presets and info;
//               or diverged (both sides have changes): nothing changed, said
//   check       `git fetch` and the counts: how far ahead and behind
//   diverged    a pull that finds both sides changed first commits the versions not yet committed, then merges the
//               remote's branch with no commit: a clean merge whose tree reads as a project is committed and saved as a
//               new version ("Merged with <sha7>"); anything else is taken back (`git merge --abort`, the folder as it
//               was) and the link marked diverged, for the user to settle, each way leaving both sides as versions:
//   keepMine    their tip saved as a version ("Theirs, <sha7>"), a merge commit that keeps this side's tree
//               (`merge -s ours`), and this side saved on top again ("Kept mine over <sha7>")
//   takeTheirs  a merge commit whose tree is theirs, saved as the newest version ("Took theirs, <sha7>"); this side
//               stays the version before
//   theirs      their tip read as a project (a checkout of the fetched commit beside the folder, taken away after),
//               for the COMPARE card: no network, nothing written to the store
//   clone       Add from Git: `git clone` into a new folder, its tree read and added as a new project (projectstore::add:
//               "<name> 2" when the name is taken), linked. The other kind's project, or a repository that holds none, is
//               refused, its clone left where the words say (a failed clone git takes away itself)
//   disconnect  the link removed; the working folder stays where it is, and the outcome says where
//
// Each returns a Result: ok, or why in words (git's own last lines as they are when git said something, with a line on
// what a sandboxed host or an SSH key with a passphrase needs when that is what it looks like). Nothing is ever forced,
// and nothing is deleted: a version removed in the store stays a commit in the repository, and no folder is ever taken
// away by this code (writeTree removes only the regular files the tree before it wrote: ProjectExport.h). The projects'
// Git folder that is a link (or a file) is refused, nothing made or changed.
#pragma once

#include "plugin/Git.h"
#include "plugin/ProjectStore.h"

#include <atomic>
#include <functional>
#include <optional>
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
        bool merged = false;                // a pull that joined both sides cleanly
        std::string theirs;                 // their tip's commit (diverged, keepMine, takeTheirs, theirs)
        std::optional<projectstore::Project> theirsProject;   // theirs: their tip as a project
    };

    Result connect(const Setup& s, const std::string& project, const std::string& remote, const std::string& branch);
    Result push(const Setup& s, const std::string& project);
    Result pull(const Setup& s, const std::string& project);
    Result check(const Setup& s, const std::string& project);
    Result clone(const Setup& s, const std::string& remote, const std::string& branch);
    Result keepMine(const Setup& s, const std::string& project);
    Result takeTheirs(const Setup& s, const std::string& project);
    Result theirs(const Setup& s, const std::string& project);
    Result disconnect(const std::string& project);

    // The folder a project's repository is made in: <projects folder>/Git/<name>-<8 hex of its id>, a number after it
    // when that is taken.
    juce::File folderFor(const std::string& project, const std::string& id);
    // The projects' Git folder, made when missing; none, with why, when it is a link or a file.
    std::optional<juce::File> gitRoot(std::string& why);
    // A remote's address shortened for the card and the mark's title: "github.com/me/pad" for
    // https://github.com/me/pad.git, git@github.com:me/pad.git and ssh://git@github.com/me/pad.
    std::string shortRemote(const std::string& address);
    // A commit's message for version `number` with its note.
    std::string messageOf(int number, const std::string& note);
}
