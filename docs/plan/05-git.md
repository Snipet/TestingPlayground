# 05. Git push and pull for projects

Planning-document item: "Add a git integration that lets users push and pull projects inside TestingPlayground to a
remote git repository. If a project has a remote repo attached to it, there should be a clean way to indicate
this." Size L. Decision D1: the user's own git, one repository per project. Needs 03 (to compare the two sides of
a diverged project) and 07 (a row in Settings).

## 1. What the user gets

- In the Projects card, **Git...** on a project: give it a remote, then **Push** and **Pull**. Each saved version
  becomes a commit; a pull that brings something new adds it as a new version.
- **Add from Git...**: a remote's address makes a new project here.
- A project with a remote shows a small branch mark after its name, in the Projects card, the explorer and the top
  bar's pill, with arrows when it is known to be ahead or behind.
- It uses the git already on the machine and whatever that git uses to sign in (an SSH agent, a credential
  helper). The plugin stores no credential and asks for none.

## 2. What exists

- A project is one JSON file, `<projects folder>/<name>.fmproject`, holding every version's texts
  (`Source/plugin/ProjectStore.h`). It does not diff or merge as text.
- Two layouts already write a project as plain files: the source export (`ProjectExport`: `project.json` with the
  texts left out, the newest version's files beside it, `versions/vN/`, `samples/`) and `factory/<key>/<name>/`
  (`project.json` and one version's files). The folder sync (`FolderSync`) mirrors only the editor's draft, into
  `<projects folder>/Folders/<name>/`.
- **Nothing starts a process and reads its output in the plugin.** `plugin/Launch.h` is fire-and-forget and POSIX
  only. The patterns to copy are `fminus/Shaders.cpp`'s file-local `run(args, log)` (CreateProcessW with a handle
  list on Windows, posix_spawn elsewhere, a timeout and a kill) and the installer's `runProgram(argv, out, err)`.
- CLAUDE.md, "Linux": never a shell between the plugin and a path. CLAUDE.md, "The network": the chat is the only
  code that reaches the network (the factory store is the stated exception).
- There is no git at run time anywhere in `Source/`. The build already requires git (`find_package(Git REQUIRED)`),
  so every machine that builds and every CI runner has one.
- The projects folder may hold hidden files (`.projects-view.json`, never listed) and subfolders (`Folders/`).

## 3. Design

### 3.1 The pieces

| Piece | Where | What |
|---|---|---|
| `run::run` | `Source/plugin/Run.h/.cpp`, JUCE-free | A program started from an argument list, never a shell: its stdout and stderr captured (each cut at a limit), its exit code, a working folder, extra environment, a timeout, a cancel that kills it. The child gets no handle of the host's (the handle list on Windows, `POSIX_SPAWN_CLOEXEC_DEFAULT` on macOS, glibc's `addclosefrom_np` on Linux, as the MCP server starts its helper) |
| `git::` | `Source/plugin/Git.h/.cpp`, JUCE-free | Finds git, runs it with the fixed environment below, and the operations of 3.4 as functions over a folder: no knowledge of projects |
| The tree | `ProjectExport`: `writeTree(project, version, folder)`, `readTree(folder)` | The repository's layout, shared with the source export's writer and reader |
| The links | `ProjectStore`: `.git-links.json` in the projects folder | Which project has which working folder, remote and branch, and what was last synced |
| The job | `Processor` (a file of its own, `ProcessorGit.cpp`) | One git job at a time on a thread of its own, as the saver; state for the editor to poll |
| `GitCard` | `Source/editor/GitCard.h/.cpp` | The card: remote, state, Push, Pull, the settling of a diverged project |

### 3.2 The repository's layout (format `funk-playground-git`, version 1)

```
project.json        the format, plugin, kind, name; info; the files' order; the dials (name, value, label);
                    presets; the MIDI map; the circuits' answers; samples as {name, file}; the save's time and note
program.fm, ...     the version's files, as named
samples/<file>      each sample as it was added
README.md           written once at Connect when there is none; never rewritten
.gitattributes      "* text=auto eol=lf" and "samples/* binary", written once
```

It is the source export's layout without `versions/` (git's history is the versions). The project's id is left
out, as exports leave it out: a clone is a project of its own on that machine.

### 3.3 Where things live

- Working folders: `<projects folder>/Git/<tidy name>-<8 hex of the project's id>/`, each a normal git
  repository the user may also open in a terminal or another client.
- `.git-links.json` (hidden, never listed, read again only when its time or size changes, written under the
  store's one writer, as the view file): `{"links": [{"id", "folder", "remote", "branch", "committed": 12,
  "commit": "<sha>", "syncedMs", "ahead": 0, "behind": 0, "checkedMs"}]}`. `committed` is the store's version
  number last made a commit. Keyed by the project's id, so a rename keeps the link.
- `projectstore::Entry` and `EditorPort::ProjectEntry` gain `git` (linked) and `ahead`/`behind` (as of the last
  fetch, -1 unknown), joined from the links: the listing reads no repository.
- Nothing about git goes into the project file, so an export never carries a remote's address.

### 3.4 The operations

Every one runs `git -C <folder> ...` on the job's thread. Refusals are sentences shown in the card.

- **Connect** (a project, a remote, a branch, default `main`): make the folder; `git init -b <branch>`; for each
  kept version from the oldest: write the tree, `git add -A`, `git commit` with the version's note as the message
  (else "Version N"), its save time as the date and a trailer `Playground-Version: N`; `git remote add origin`;
  `git fetch origin`. A remote branch that already has commits is refused: "This repository is not empty. Use Add
  from Git to make a project of it, or give an empty repository." Else `git push -u origin <branch>`.
- **Push**: a commit for each version newer than `committed`; `git push origin <branch>`. Rejected because the
  remote moved: the state becomes "behind: pull first", nothing forced. **Never `--force`.**
- **Pull**: `git fetch origin <branch>`, then one of:
  - nothing new;
  - **fast-forward** (`git merge --ff-only`): the tree is read and saved as a new version, note "Pulled <sha7>:
    <subject>" (and "and N more commits"). If the project is open with no unsaved change, the new version is
    opened through the same path the Projects card's Open uses; else it waits in the versions list;
  - **diverged** (both sides have commits): first commit any versions not yet committed, then
    `git merge --no-commit --no-ff origin/<branch>`. Clean: commit the merge and save its tree as a new version
    ("Merged with <sha7>"). Conflicts: `git merge --abort`, and the card offers three ways, none of which loses
    anything, since both sides end as versions in the store:
    - **Keep mine**: their tip is saved as a version ("Theirs, <sha7>"), then a merge commit that keeps this
      side's tree (`git merge -s ours`), then mine saved on top again;
    - **Take theirs**: a merge commit with their tree, saved as the newest version; mine stays the version before;
    - **Compare...**: package 03's card, mine against theirs (their tree read from the fetched commit), to decide.
- **Check** (the card's refresh): `git fetch`, then `git rev-list --left-right --count HEAD...origin/<branch>`.
- **Add from Git** (a remote): `git clone` into a new folder; the tree read and added as a new project
  (`projectstore::add`, so a taken name gets a number), linked. The other product's project is refused in words.
- **Disconnect**: the link is removed. The working folder stays where it is and the card says where (nothing is
  deleted by the plugin).

### 3.5 Running git safely

- **Finding it:** the path set in Settings, else `<PREFIX>GIT` (the probes'), else the first that exists of the
  platform's places (macOS: `/opt/homebrew/bin/git`, `/usr/local/bin/git`, and `/usr/bin/git` only when
  `xcode-select -p` succeeds, since without the command line tools that shim opens Apple's install dialog;
  Windows: `git.exe` on the PATH, `%ProgramFiles%\Git\cmd\git.exe`; Linux: the PATH). None: the card says "Git was
  not found on this computer" with where to get it, and every other part of the plugin is as before.
- **The environment of every run:** `GIT_TERMINAL_PROMPT=0` (never a prompt on a terminal that is not there);
  `GIT_SSH_COMMAND="ssh -o BatchMode=yes"` unless the user's environment sets one (ssh fails at once instead of
  waiting for a passphrase); `GIT_ALLOW_PROTOCOL=https:ssh` (and `file` in the probes) and
  `-c protocol.ext.allow=never`, so a remote address can never be a command (`ext::`); `-c core.hooksPath=` an
  empty folder of the plugin's, so no hook runs; `-c core.fsmonitor=false`; `LC_ALL=C` so the output parses. A
  credential helper that opens its own window (Git Credential Manager) may: the user pressed the button.
- **The remote's address** is `https://...`, `ssh://...` or `user@host:path`; one that starts with `-`, holds a
  control character or names another scheme is refused before git sees it; it is passed after `--`.
- **Never a shell**, every path an argument. A timeout (120 s for the network operations, 30 s for the others) and
  the card's Cancel kill the child. A processor that goes cancels and joins the job.
- **Who commits:** git's own `user.name` and `user.email`. Unset: the commit fails, and the card says so with the
  two commands that set them.
- **Never by itself** (assumption A12): no fetch on a timer, on opening a project or an editor. CLAUDE.md's
  network rule gains: "git, when the user presses Connect, Push, Pull, Check or Add from Git, reaches the remote
  the user gave".
- What a pull brings is a program the plugin will compile and play when opened: the same trust as a project file
  the user imports. The card says whose repository it pulls from.

### 3.6 The card and the mark

- `GitCard`, from the Projects card's **Git...** (beside Export...) and the project pill's menu. Not linked: a
  field for the remote, a field for the branch, **Connect**; and **Add from Git...** in the Projects card beside
  Import.... Linked: the remote (shortened: `github.com/me/pad`), the branch, "3 versions to push", "2 commits to
  pull" or "Up to date, checked 14:02", the last eight commits (subject, when), **Push**, **Pull**, **Check**,
  **Show folder**, **Disconnect**, Close. While a job runs: what it is doing, and Cancel. After: its outcome, or
  git's own words when it failed (its stderr's last lines, as they are).
- The mark: a small branch glyph (`ChatStyle.h`'s icons gain one) after the name in the card's rows, the
  explorer's PROJECTS rows and the top bar's pill; an up or down arrow with a count when known. Its accessibility
  title: "linked to <remote>".
- `EditorPort`: `gitState(project)`, `startGit(job)`, `cancelGit()`, `gitPolled()` (a revision, as
  `projectsRead`).

## 4. Work, in three pull requests

1. **`Run` and `Git`** with their probe, no UI: connect, push, pull (fast-forward), check, clone against `file://`
   remotes.
2. **The store's links, the job, the card, the mark:** Connect, Push, Pull, Check, Add from Git, Disconnect.
3. **Diverged projects:** the merge, Keep mine, Take theirs, Compare (after package 03).

## 5. Tests

- New probe `plugin.git`, with bare repositories in its sandbox as remotes and two store folders standing for two
  machines (`projectstore::setDirectoryForTests`); git's own configuration pointed at files of the probe
  (`GIT_CONFIG_GLOBAL`, `GIT_CONFIG_SYSTEM`), the committer set by environment. Sections: RUN (output captured,
  the exit code, a timeout kills, a cancel kills, no handle inherited), CONNECT (a commit a version, in order,
  with its note, date and trailer; a non-empty remote refused), PUSH (only the new versions; rejected when
  behind, nothing forced), PULL (nothing new; fast-forward into a new version; open and unedited: opened; edited:
  left), MERGE (clean; conflicts aborted and the tree as before; Keep mine and Take theirs each leave both sides
  as versions), CLONE (a project of its own, the other kind refused), LINKS (the file round-trips; a rename keeps
  the link; the listing reads no repository), ADDRESS (`ext::`, a leading dash, a control character refused),
  HOOKS (a planted hook does not run), NOGIT (a path that is no git: said, nothing else changes), TREE (the
  layout reads back to the same version: files, dials with labels, presets, samples).
  With no git on the machine the probe skips, saying so (`probe::skip`).
- New probe `gui.git`: the card not linked, linked, running, failed, diverged (its three ways), in both themes;
  the mark in the three places; the keys and accessibility.
- `cmake/Probes.cmake` gives the probes `<PREFIX>GIT` (CMake's `GIT_EXECUTABLE`). Nothing reaches a network.

## 6. Docs

README: a section "Projects and git" (what it does, what it needs, where the folders are, what a pull does to
versions, how a diverged project is settled, how to sign in: git's own ways). CHANGELOG: Added. CLAUDE.md: rows
for `Run`, `Git`, `GitCard`; a section "Git" with the rules of 3.5; "The network" amended; `.git-links.json`
beside the view file.

## 7. Risks

- **Sandboxed hosts.** An Audio Unit in a sandboxed host (GarageBand; Logic's separate plugin process) may not be
  allowed to start git, read `~/.ssh` or reach the network. The feature must fail soft: git's refusal shown, and a
  line that the Standalone can push and pull. Try it there before promising it in a host.
- **A passphrase-protected SSH key with no agent** fails at once (BatchMode). The words say to add the key to the
  agent or use HTTPS with a credential helper.
- **Large samples** make large repositories (64 MB a project at most). No git-lfs in this plan.
- **The 100-version cap** and git's history are different things: a version removed here stays a commit there.
  The card says the history is the repository's.
- A user who edits the working folder by hand and commits there is pulling from themselves: a pull reads the tree
  as it is in the fetched commit, and a tree that does not read as a project (no `project.json`) is refused, the
  folder left as it was.
