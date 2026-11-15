// Source/plugin/Git.h: the user's own git, run over a folder of ordinary files (JUCE-free). It knows nothing of
// projects: what goes into the folder, and what a commit means to the store, is its caller's. Every run goes through
// plugin/Run.h (an argument list, never a shell; no handle of the host's) with the fixed environment and options
// CLAUDE.md's "Git" section gives (no prompt, ssh in batch mode, only https and ssh remotes, no `ext::`, no hook, no
// fsmonitor, no automatic gc, C's messages), a timeout (kNetworkMs for what reaches a remote, kLocalMs for the rest)
// and the caller's cancel flag. Nothing is thrown: a failure comes back as an Outcome whose `why` holds git's own last
// lines, as they are. Calls block for as long as git runs: never on the audio or the message thread.
//
// Finding git (find()): the path given (the user's, from Settings), else <PREFIX>GIT (the probes': CMake's
// GIT_EXECUTABLE), else the platform's places: macOS /opt/homebrew/bin/git, /usr/local/bin/git, then /usr/bin/git only
// when `xcode-select -p` succeeds (without the command line tools that shim opens Apple's install dialog); Windows
// git.exe on the PATH, then %ProgramFiles%\Git\cmd\git.exe; Linux the PATH. None found is a state (Found::path empty,
// `why` in words), not an error; a path that is no git (its `--version` does not say "git version") is not taken.
#pragma once

#include "plugin/Run.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace playground::git
{
    inline constexpr int kNetworkMs = 120000;      // fetch, push, clone, ls-remote
    inline constexpr int kLocalMs = 30000;         // everything else

    struct Found
    {
        std::string path;       // "" when none
        std::string version;    // "2.43.0"
        std::string why;        // none found, or the one given is no git: in words
        bool found() const { return !path.empty(); }
    };

    // `given`: the path the user set ("" none). The words for none: notFoundWords().
    Found find(const std::string& given = {});
    std::string notFoundWords();

    // What every call runs with.
    struct Context
    {
        std::string git;                               // its path (Found::path)
        std::string folder;                            // the working folder: git -C <folder>
        const std::atomic<bool>* cancel = nullptr;     // set: the running git is killed
        bool allowFile = false;                        // GIT_ALLOW_PROTOCOL gains file (the probes' file:// remotes)
        int localMs = kLocalMs, networkMs = kNetworkMs;
        std::size_t outLimit = std::size_t{ 1 } << 20; // git's stdout kept (show() reads a sample's bytes through it)
    };

    struct Outcome
    {
        bool ok = false;
        std::string why;          // !ok: in words, git's own last lines when git said something
        std::string out;          // git's stdout (cut at run::Request's limit)
        run::Ended ended = run::Ended::notStarted;
        int code = -1;
        bool behind = false;      // push: refused because the remote has commits this one has not (pull first)
        bool outCut = false;      // git wrote more than Context::outLimit: `out` holds its first bytes
    };

    // The remote's address: https://..., ssh://... or user@host:path (and file://... when `allowFile`). "" when it may
    // be used, else why not: one that starts with '-', holds a control character or a space, names another scheme or
    // a transport (`ext::`, `fd::`), or is a plain path, is refused before git sees it.
    std::string addressRefusal(const std::string& address, bool allowFile = false);

    // A branch's name as this code takes it ("" fine): letters, digits and . _ / -, not starting with '-' or '.', no
    // "..", no "//", not ending in '/', ".lock" or '.'.
    std::string branchRefusal(const std::string& branch);

    // git <fixed options> -C <folder> <args...>, with the fixed environment; `network`: its longer timeout. The base
    // of the rest, for what they do not cover (the probes read commits' bodies through it).
    Outcome call(const Context& c, const std::vector<std::string>& args, bool network = false);

    // `git init` in the folder (made when missing), its first branch `branch`.
    Outcome init(const Context& c, const std::string& branch);

    // Everything in the folder added (`add -A`) and committed: `message`, then a blank line and each trailer as
    // "Key: value" (a key of letters, digits and '-'; one line each); `whenMs` (ms since 1970, 0: now) its author's
    // and committer's date. With nothing changed the commit is made all the same (`allowEmpty`) or refused. `sha`: the
    // commit made. Git's own user.name and user.email commit; unset, it fails, and `why` says how to set them.
    Outcome commitAll(const Context& c, const std::string& message, std::int64_t whenMs,
                      const std::vector<std::pair<std::string, std::string>>& trailers, std::string* sha = nullptr,
                      bool allowEmpty = true);

    // `git remote add <name> -- <address>` (the address checked first).
    Outcome addRemote(const Context& c, const std::string& name, const std::string& address);

    // Whether the remote has `branch` (git ls-remote): `has`, and its commit in `sha`.
    Outcome remoteHas(const Context& c, const std::string& remote, const std::string& branch, bool& has,
                      std::string* sha = nullptr);

    // refs/remotes/<remote>/<branch> brought up to date from the remote's branch.
    Outcome fetch(const Context& c, const std::string& remote, const std::string& branch);

    // The local branch to the remote's (`setUpstream`: -u). Never forced: a remote that moved refuses it, and the
    // Outcome says `behind` (pull first).
    Outcome push(const Context& c, const std::string& remote, const std::string& branch, bool setUpstream = false);

    // Commits on HEAD the remote's branch (as last fetched) lacks, and the other way round. A side that is missing
    // (no commit yet, never fetched) counts its commits against nothing.
    struct Counts
    {
        int ahead = 0, behind = 0;
    };
    Outcome counts(const Context& c, const std::string& remote, const std::string& branch, Counts& counts);

    // HEAD fast-forwarded to the remote's branch as last fetched; refused (nothing changed) when it is no fast-forward.
    Outcome fastForward(const Context& c, const std::string& remote, const std::string& branch);

    // `git clone` of `address` into `folder` (its parent must exist; the folder must not, or be empty), its `branch`
    // when one is given. The Context's own folder is not used.
    Outcome clone(const Context& c, const std::string& address, const std::string& folder,
                  const std::string& branch = {});

    // The last `n` commits on HEAD, newest first; none (and ok) in a repository with no commit yet.
    struct Commit
    {
        std::string sha;
        std::string subject;
        std::int64_t timeMs = 0;    // its author's date
    };
    Outcome log(const Context& c, int n, std::vector<Commit>& commits);
    // The same over `range` ("HEAD..refs/remotes/origin/main", a commit): newest first; none (and ok) when it is empty.
    Outcome log(const Context& c, int n, std::vector<Commit>& commits, const std::string& range);

    // A file's bytes as `rev` has it (`git cat-file blob <rev>:<path>`), whatever the working folder holds: refused when
    // it is larger than Context::outLimit. The files `rev` holds (`git ls-tree -r`), as paths with '/'.
    Outcome show(const Context& c, const std::string& rev, const std::string& path, std::string& bytes);
    Outcome files(const Context& c, const std::string& rev, std::vector<std::string>& paths);

    // `rev`'s commit ("" when it names none here); treeOf: its tree's id.
    std::string resolve(const Context& c, const std::string& rev);
    std::string treeOf(const Context& c, const std::string& rev);
    // The branch HEAD is on ("" detached or none).
    std::string currentBranch(const Context& c);

    // HEAD's commit ("" none yet).
    Outcome head(const Context& c, std::string& sha);

    // ---- Package 05, part 3: two sides that both moved -----------------------------------------------------------------
    // The merge of `ours` and `theirs` worked out in git's object store alone (`git merge-tree --write-tree`: the working
    // folder, the index and HEAD untouched, nothing to clear after a cancel or a crash): `clean`, the merged tree's id,
    // and the files that conflict. ok when git ran it, clean or not. A git older than 2.38 cannot: `unsupported`.
    struct Merged
    {
        bool clean = false;
        bool unsupported = false;
        std::string tree;
        std::vector<std::string> conflicts;
    };
    Outcome mergeTree(const Context& c, const std::string& ours, const std::string& theirs, Merged& out);
    // A commit of `tree` over `parents` (`git commit-tree`), its message and date (as commitAll's), made in the object
    // store alone: `sha` the commit. fastForwardTo then moves HEAD, the index and the working folder to it (`merge
    // --ff-only`: refused, nothing changed, when it is no fast-forward or a file the folder holds would be overwritten).
    Outcome commitTree(const Context& c, const std::string& tree, const std::vector<std::string>& parents,
                       const std::string& message, std::int64_t whenMs, std::string& sha);
    Outcome fastForwardTo(const Context& c, const std::string& commit);
    // A merge left under way in the folder (MERGE_HEAD: the user's own git there); `ancestor` is in `of`'s history.
    bool mergeUnderWay(const Context& c);
    bool isAncestor(const Context& c, const std::string& ancestor, const std::string& of);
}
