#!/usr/bin/env python3
"""
The changelog's entries: one file a pull request in docs/changelog/, folded into CHANGELOG.md on main, so that no two
pull requests edit the same lines of CHANGELOG.md (each merge to main used to make every other open one conflict at
the top of its Unreleased section). Subcommands:

  check         Hold every entry file to the rules below, and CHANGELOG.md to having its `## Unreleased` heading and
                its releases' headings well formed, newest first.
  show          Print the Unreleased section as it would be once the entries are folded in. Changes nothing.
  fold          Move every entry into CHANGELOG.md, at the top of `## Unreleased`, and delete the entry files.
  cut           fold, then move everything under `## Unreleased` into a new `## YYYY-MM-DD` section below it.
  release       cut (when anything is under Unreleased), then give the days since the last release their version's
                heading, `## Version X.Y.Z (YYYY-MM-DD)`, under `## Unreleased`.
  check-release Refuse a release tag (vX.Y.Z) whose version has no heading, and any tag while an entry file is left in
                docs/changelog/ (release.yml runs it on a pushed tag). A pre-release's tag (vX.Y.Z-rc.1) needs no heading.
  selftest      Run this script's own tests in a temporary git repository of made-up entries (no build needed).

By example:

  scripts/changelog.py check
  scripts/changelog.py show
  scripts/changelog.py fold                     on main, after the day's merges; then commit what it says
  scripts/changelog.py cut --date 2026-11-12    the day's section made of everything under Unreleased
  scripts/changelog.py release --version 0.2.0 --date 2026-11-20    on main, before tagging v0.2.0
  scripts/changelog.py check-release --tag v0.2.0

A release's heading covers everything under it down to the next release's heading: the day sections cut since the
release before (`## Unreleased` stays on top, empty). The plugins compile CHANGELOG.md in and show the version they are
(their What's new); a build whose version has no heading shows Unreleased.

An entry is docs/changelog/<name>.md, <name> lower-case letters, digits and hyphens (README.md there is never one).
It holds what its pull request changed for users, as CHANGELOG.md's entries do: a `### Title` heading, then any of
the `**Added**`, `**Fixed**` and `**Changed**` lists, each of `- ` items, with no `# ` or `## ` heading, no tab, no
line over 120 columns, and one newline at its end. A pull request that raises the Claude Desktop extension's version
starts it with `Claude Desktop extension: **0.18.0** (was 0.17.0).` and a blank line.

fold's order: the entries not yet committed come first, by name; then the others, newest first by the time of the
commit that brought each file onto the current branch (along its first-parent line: a pull request's merge, or a
commit made on the branch itself; the newest such commit when a file was removed and added again), ties by name.

An entry whose heading has no pull request link gets one when that commit is a "Merge pull request #N from ..." merge
(the link built from the `origin` remote's owner and name); else its heading is left as it is. The extension lines
become one, at the top of Unreleased: the highest new version, "was" the lowest old one, a line already there among
them. CHANGELOG.md is written through a temporary file renamed into place, with the line endings it has; check and
fold read a file's CR LF as LF (a Windows checkout may hold them so) and check refuses a CR alone. The entries are
removed with `git rm` in a git checkout (unlinked otherwise). fold and cut commit nothing: they say what to commit.

Stdlib only; no network; git only to read the history, and `git rm`.
"""

from __future__ import annotations

import argparse
import datetime
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_PATH = Path(__file__).resolve()
PROJECT_ROOT = SCRIPT_PATH.parent.parent

CHANGELOG = Path("CHANGELOG.md")
ENTRIES_DIR = Path("docs") / "changelog"
README = "README.md"
UNRELEASED = "## Unreleased"
MAX_COLUMNS = 120
LISTS = ("**Added**", "**Fixed**", "**Changed**")

NAME_RE = re.compile(r"^[a-z0-9-]+\.md$")
EXTENSION_RE = re.compile(r"^Claude Desktop extension: \*\*(\d+)\.(\d+)\.(\d+)\*\* \(was (\d+)\.(\d+)\.(\d+)\)\.$")
LINK_RE = re.compile(r" \(\[#\d+\]\([^()\s]+\)\)$")
MERGE_RE = re.compile(r"^Merge pull request #(\d+) from ")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")
TAG_RE = re.compile(r"^v(\d+\.\d+\.\d+)(-[0-9A-Za-z.-]+)?$")
RELEASE_RE = re.compile(r"^## Version (\d+\.\d+\.\d+) \((\d{4}-\d{2}-\d{2})\)[ \t]*$")

Version = tuple[int, int, int]


# ---- reading ------------------------------------------------------------------------------------------------------

class Entry:
    def __init__(self, path: Path, extension: tuple[Version, Version] | None, body: str) -> None:
        self.path = path
        self.extension = extension  # (new, was)
        self.body = body            # from its `### ` heading, with no newline at its end
        self.time: int | None = None


def version_text(v: Version) -> str:
    return ".".join(str(n) for n in v)


def extension_line(new: Version, was: Version) -> str:
    return f"Claude Desktop extension: **{version_text(new)}** (was {version_text(was)})."


def parse_extension(line: str) -> tuple[Version, Version] | None:
    m = EXTENSION_RE.match(line)
    if not m:
        return None
    n = [int(g) for g in m.groups()]
    return (n[0], n[1], n[2]), (n[3], n[4], n[5])


def entry_files(root: Path) -> list[Path]:
    """Every file in docs/changelog/ but its README, sorted by name (subfolders' files too: check refuses them)."""
    folder = root / ENTRIES_DIR
    if not folder.is_dir():
        return []
    return sorted((p for p in folder.rglob("*") if p.is_file() and p != folder / README),
                  key=lambda p: p.relative_to(folder).as_posix())


def problems_of(root: Path, path: Path) -> list[str]:
    """What is wrong with one entry file, each as `<file>:<line>: <problem>` (the file's line 1 when it is the file)."""
    rel = path.relative_to(root).as_posix()
    out: list[str] = []

    def bad(line: int, what: str) -> None:
        out.append(f"{rel}:{line}: {what}")

    if path.parent != root / ENTRIES_DIR or not NAME_RE.match(path.name):
        bad(1, "not an entry's name: docs/changelog/<name>.md, the name lower-case letters, digits and hyphens")
        return out
    try:
        text = path.read_bytes().decode("utf-8").replace("\r\n", "\n")
    except UnicodeDecodeError:
        bad(1, "not UTF-8")
        return out
    if not text:
        bad(1, "empty")
        return out
    if not text.endswith("\n"):
        bad(text.count("\n") + 1, "no newline at the end of the file")
    elif text.endswith("\n\n"):
        bad(text.count("\n"), "more than one newline at the end of the file")
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()

    headings = 0
    current = None          # the list heading the lines below belong to, and its line
    items: dict[str, int] = {}
    seen_list: dict[str, int] = {}
    first = 0
    if lines and lines[0].startswith("Claude Desktop extension"):
        if parse_extension(lines[0]) is None:
            bad(1, "the extension's line is not `Claude Desktop extension: **X.Y.Z** (was X.Y.Z).`")
        if len(lines) < 2 or lines[1] != "":
            bad(2, "a blank line must follow the extension's line")
        first = 2
        while first < len(lines) and lines[first] == "":
            first += 1
    elif lines:
        while first < len(lines) and lines[first] == "":
            first += 1
    for i, line in enumerate(lines, start=1):
        if "\t" in line:
            bad(i, "a tab (indent with spaces)")
        if "\r" in line:
            bad(i, "a carriage return alone (end lines with LF, or CR LF)")
        if len(line) > MAX_COLUMNS:
            bad(i, f"{len(line)} columns: wrap it at {MAX_COLUMNS}, as CHANGELOG.md is")
        if i > 1 and line.startswith("Claude Desktop extension"):
            bad(i, "the extension's line goes first, before the heading")
        if re.match(r"^#{1,2}(\s|$)", line):
            bad(i, "a `# ` or `## ` heading: an entry has one `### ` heading (fold places it under a day's section)")
        elif line.startswith("### "):
            headings += 1
            if i - 1 != first:
                bad(i, "the `### ` heading comes first (after the extension's line, when there is one)")
            if headings > 1:
                bad(i, "a second `### ` heading: one pull request, one entry")
        elif line.startswith("####"):
            bad(i, "a heading below `### `: use the **Added**, **Fixed** and **Changed** lists")
        elif line in LISTS:
            if line in seen_list:
                bad(i, f"{line} again (first on line {seen_list[line]}): one list of each")
            seen_list[line] = i
            items[line] = 0
            current = line
        elif line.startswith("- ") and current is not None:
            items[current] += 1
    if headings == 0:
        bad(first + 1 if first < len(lines) else 1, "no `### ` heading: an entry starts with `### <title>`")
    if not seen_list:
        bad(1, "no **Added**, **Fixed** or **Changed** list")
    for name, n in items.items():
        if n == 0:
            bad(seen_list[name], f"{name} has no `- ` item")
    return out


def read_entry(root: Path, path: Path) -> Entry:
    lines = path.read_bytes().decode("utf-8").replace("\r\n", "\n").rstrip("\n").split("\n")
    extension = None
    if lines and parse_extension(lines[0]) is not None:
        extension = parse_extension(lines[0])
        lines = lines[1:]
    while lines and lines[0] == "":
        lines = lines[1:]
    return Entry(path, extension, "\n".join(lines))


def read_changelog(root: Path) -> str:
    """CHANGELOG.md's text with its CR LF read as LF (crlf() says whether it had them, for writing it back so)."""
    return (root / CHANGELOG).read_bytes().decode("utf-8").replace("\r\n", "\n")


def crlf(root: Path) -> bool:
    return b"\r\n" in (root / CHANGELOG).read_bytes()


def split_changelog(text: str) -> tuple[str, str, str] | None:
    """(everything up to and including the `## Unreleased` line's newline, its section's body, the rest from the next
    `## ` heading), or None when there is no `## Unreleased` heading."""
    m = re.search(r"^## Unreleased[ \t]*\n", text, re.MULTILINE)
    if not m:
        if re.search(r"^## Unreleased[ \t]*\Z", text, re.MULTILINE):
            return text + "\n", "", ""
        return None
    head = text[:m.end()]
    after = text[m.end():]
    n = re.search(r"^## ", after, re.MULTILINE)
    if n:
        return head, after[:n.start()], after[n.start():]
    return head, after, ""


def release_headings(text: str) -> list[tuple[int, str, Version | None, str | None]]:
    """Every line that starts `## Version`, in the file's order: its line number, the line, and its version and date
    when it is well formed (`## Version X.Y.Z (YYYY-MM-DD)`), else None for both."""
    out = []
    for n, line in enumerate(text.split("\n"), start=1):
        if not line.startswith("## Version"):
            continue
        m = RELEASE_RE.match(line)
        if m:
            out.append((n, line, parse_version(m.group(1)), m.group(2)))
        else:
            out.append((n, line, None, None))
    return out


def parse_version(text: str) -> Version | None:
    m = VERSION_RE.match(text)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def is_day(date: str) -> bool:
    if not DATE_RE.match(date):
        return False
    try:
        datetime.date.fromisoformat(date)
    except ValueError:
        return False
    return True


def release_problems(text: str) -> list[str]:
    """What is wrong with CHANGELOG.md's releases' headings: each well formed, its date a day, below `## Unreleased`,
    newest first, each version once."""
    out: list[str] = []
    unreleased = re.search(r"^## Unreleased[ \t]*$", text, re.MULTILINE)
    top = text[:unreleased.start()].count("\n") + 1 if unreleased else 0
    last: tuple[Version, int] | None = None
    for n, line, version, date in release_headings(text):
        if version is None or date is None:
            out.append(f"{CHANGELOG}:{n}: not a release's heading: `## Version X.Y.Z (YYYY-MM-DD)`")
            continue
        if not is_day(date):
            out.append(f"{CHANGELOG}:{n}: {date} is not a day of the calendar")
        if unreleased and n < top:
            out.append(f"{CHANGELOG}:{n}: a release's heading above `{UNRELEASED}` (it stays on top)")
        if last is not None:
            if version == last[0]:
                out.append(f"{CHANGELOG}:{n}: Version {version_text(version)} again (first on line {last[1]})")
            elif version > last[0]:
                out.append(f"{CHANGELOG}:{n}: Version {version_text(version)} below {version_text(last[0])} "
                           f"(line {last[1]}): the newest release first")
        if last is None or version < last[0]:
            last = (version, n)
    return out


# ---- git ----------------------------------------------------------------------------------------------------------

def git(root: Path, *args: str) -> str | None:
    """git's output in the checkout at root, or None when git fails (or there is no git)."""
    try:
        r = subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True, check=False)
    except OSError:
        return None
    return r.stdout if r.returncode == 0 else None


def in_git(root: Path) -> bool:
    top = git(root, "rev-parse", "--show-toplevel")
    return top is not None and Path(top.strip()).resolve() == root.resolve()


def repository_url(root: Path) -> str | None:
    """https://<host>/<owner>/<name> from the origin remote (https, ssh or scp-like), else None."""
    url = git(root, "remote", "get-url", "origin")
    if not url:
        return None
    url = url.strip()
    m = (re.match(r"^https?://(?:[^@/]+@)?([^/:]+)(?::\d+)?/([^/]+)/([^/]+?)(?:\.git)?/?$", url)
         or re.match(r"^ssh://(?:[^@/]+@)?([^/:]+)(?::\d+)?/([^/]+)/([^/]+?)(?:\.git)?/?$", url)
         or re.match(r"^(?:[^@/]+@)?([^/:]+):([^/]+)/([^/]+?)(?:\.git)?/?$", url))
    if not m:
        return None
    return f"https://{m.group(1)}/{m.group(2)}/{m.group(3)}"


def added_by(root: Path, path: Path) -> tuple[int, str] | None:
    """The commit that brought the file onto the current branch, seen along its first-parent line with each merge
    diffed against its first parent (a pull request's merge, or a commit made on the branch itself; the newest when
    the file was removed and added again): its commit time and subject. None when it is not committed there."""
    out = git(root, "log", "--first-parent", "-m", "--diff-filter=A", "--format=%ct %s", "-n", "1", "--",
              path.relative_to(root).as_posix())
    if not out or not out.strip():
        return None
    ct, _, subject = out.strip().split("\n")[0].partition(" ")
    return int(ct), subject


# ---- folding ------------------------------------------------------------------------------------------------------

class Problem(Exception):
    pass


def gather(root: Path) -> list[Entry]:
    """Every entry, checked (a Problem when one is not well formed), newest first, its heading linked where it can."""
    files = entry_files(root)
    problems = [p for f in files for p in problems_of(root, f)]
    if problems:
        raise Problem("\n".join(problems) + "\nchangelog.py: fix the entries first (scripts/changelog.py check)")
    git_ok = in_git(root)
    url = repository_url(root) if git_ok else None
    entries = []
    for f in files:
        e = read_entry(root, f)
        added = added_by(root, f) if git_ok else None
        if added:
            e.time = added[0]
            heading, _, rest = e.body.partition("\n")
            if not LINK_RE.search(heading) and url:
                m = MERGE_RE.match(added[1])
                if m:
                    n = m.group(1)
                    e.body = f"{heading} ([#{n}]({url}/pull/{n}))" + ("\n" + rest if rest else "")
        entries.append(e)
    # Not yet committed first (the newest), then the newest to reach the branch first; ties by name.
    entries.sort(key=lambda e: (e.time is not None, -(e.time or 0), e.path.name))
    return entries


def folded_body(entries: list[Entry], body: str) -> str:
    """The Unreleased section's body with the entries on top: an extension line, then the entries, then what was
    there, each part apart by a blank line; it starts with a blank line and ends with one when it holds anything."""
    old = body.strip("\n")
    extensions = [e.extension for e in entries if e.extension]
    lines = old.split("\n") if old else []
    if lines and parse_extension(lines[0]) is not None:
        extensions.append(parse_extension(lines[0]))
        old = "\n".join(lines[1:]).strip("\n")
    parts = []
    if extensions:
        parts.append(extension_line(max(x[0] for x in extensions), min(x[1] for x in extensions)))
    parts += [e.body for e in entries]
    if old:
        parts.append(old)
    if not parts:
        return "\n"
    return "\n" + "\n\n".join(parts) + "\n\n"


def write_atomic(path: Path, text: str, crlf: bool = False) -> None:
    if crlf:
        text = text.replace("\n", "\r\n")
    fd, tmp = tempfile.mkstemp(prefix="." + path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(text.encode("utf-8"))
        shutil.copymode(path, tmp)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def remove_entries(root: Path, entries: list[Entry]) -> None:
    git_ok = in_git(root)
    for e in entries:
        rel = e.path.relative_to(root).as_posix()
        if git_ok and git(root, "ls-files", "--error-unmatch", "--", rel) is not None:
            if git(root, "rm", "-q", "-f", "--", rel) is not None:
                continue
        e.path.unlink()


def changelog_parts(root: Path) -> tuple[str, str, str]:
    if not (root / CHANGELOG).is_file():
        raise Problem(f"changelog.py: no {CHANGELOG} in {root}")
    parts = split_changelog(read_changelog(root))
    if parts is None:
        raise Problem(f"{CHANGELOG}: no `{UNRELEASED}` heading (the entries are folded in under it)")
    return parts


def describe(entries: list[Entry]) -> str:
    return "\n".join(f"  {e.path.name}: {e.body.partition(chr(10))[0]}" for e in entries)


# ---- the commands -------------------------------------------------------------------------------------------------

def cmd_check(args: argparse.Namespace) -> int:
    root = args.root
    problems = [p for f in entry_files(root) for p in problems_of(root, f)]
    if not (root / CHANGELOG).is_file():
        problems.append(f"{CHANGELOG}:1: missing")
    elif split_changelog(read_changelog(root)) is None:
        problems.append(f"{CHANGELOG}:1: no `{UNRELEASED}` heading (the entries are folded in under it)")
    else:
        problems += release_problems(read_changelog(root))
    for p in problems:
        print(p)
    if problems:
        return 1
    n = len(entry_files(root))
    print(f"changelog.py: {n} entr{'y' if n == 1 else 'ies'} in {ENTRIES_DIR.as_posix()}/, all well formed; "
          f"{CHANGELOG} has `{UNRELEASED}`" + releases_said(read_changelog(root)))
    return 0


def releases_said(text: str) -> str:
    n = len(release_headings(text))
    return "" if n == 0 else " and 1 release's heading" if n == 1 else f" and {n} releases' headings"



def cmd_show(args: argparse.Namespace) -> int:
    root = args.root
    try:
        _, body, _ = changelog_parts(root)
        entries = gather(root)
    except Problem as p:
        print(p, file=sys.stderr)
        return 1
    shown = folded_body(entries, body).rstrip("\n")
    sys.stdout.write(UNRELEASED + "\n" + (shown + "\n" if shown else ""))
    return 0


def fold(root: Path, date: str | None) -> int:
    try:
        head, body, rest = changelog_parts(root)
        entries = gather(root)
    except Problem as p:
        print(p, file=sys.stderr)
        return 1
    if date is not None:
        if not DATE_RE.match(date):
            print(f"changelog.py: {date} is no date: give it as YYYY-MM-DD", file=sys.stderr)
            return 1
        try:
            datetime.date.fromisoformat(date)
        except ValueError:
            print(f"changelog.py: {date} is not a day of the calendar", file=sys.stderr)
            return 1
        if re.search(rf"^## {re.escape(date)}[ \t]*$", read_changelog(root), re.MULTILINE):
            print(f"changelog.py: {CHANGELOG} has a `## {date}` section already", file=sys.stderr)
            return 1
    new_body = folded_body(entries, body)
    if date is not None:
        if new_body.strip("\n") == "":
            print(f"changelog.py: nothing under `{UNRELEASED}` and no entry to fold: no section to cut",
                  file=sys.stderr)
            return 1
        new_body = f"\n## {date}\n" + new_body
    elif not entries:
        print(f"changelog.py: no entry in {ENTRIES_DIR.as_posix()}/: nothing to fold, nothing changed")
        return 0
    if not rest:
        new_body = new_body.rstrip("\n") + "\n"
    write_atomic(root / CHANGELOG, head + new_body + rest, crlf(root))
    remove_entries(root, entries)
    if entries:
        print(f"Folded {len(entries)} entr{'y' if len(entries) == 1 else 'ies'} into {CHANGELOG}, newest first, "
              f"and removed {'it' if len(entries) == 1 else 'them'}:\n{describe(entries)}")
    if date is not None:
        print(f"Moved everything under `{UNRELEASED}` into `## {date}`.")
    paths = " ".join([CHANGELOG.as_posix()] + [e.path.relative_to(root).as_posix() for e in entries])
    print(f"Commit {CHANGELOG}" + (" and the removals" if entries else "") + f" (git add -A -- {paths}).")
    return 0


def cmd_fold(args: argparse.Namespace) -> int:
    return fold(args.root, None)


def cmd_cut(args: argparse.Namespace) -> int:
    return fold(args.root, args.date)


def cmd_release(args: argparse.Namespace) -> int:
    """cut when anything waits (entries, or text under Unreleased), then the release's heading under `## Unreleased`,
    over every day section since the release before. Refused, nothing changed: a version that is no X.Y.Z, one not above
    the newest release's, a heading there already, a date that is no day, nothing to release, a bad entry."""
    root = args.root
    version = parse_version(args.version)
    if version is None:
        print(f"changelog.py: {args.version} is no release's version: X.Y.Z (a pre-release gets no heading)",
              file=sys.stderr)
        return 1
    if not is_day(args.date):
        print(f"changelog.py: {args.date} is no day: give it as YYYY-MM-DD", file=sys.stderr)
        return 1
    try:
        _, body, rest = changelog_parts(root)
        entries = gather(root)
    except Problem as p:
        print(p, file=sys.stderr)
        return 1
    text = read_changelog(root)
    problems = release_problems(text)
    if problems:
        print("\n".join(problems) + "\nchangelog.py: fix the releases' headings first", file=sys.stderr)
        return 1
    known = [v for _, _, v, _ in release_headings(text) if v is not None]
    if version in known:
        print(f"changelog.py: {CHANGELOG} has a heading for Version {args.version} already", file=sys.stderr)
        return 1
    if known and version < known[0]:
        print(f"changelog.py: Version {args.version} is below the newest release, {version_text(known[0])}",
              file=sys.stderr)
        return 1
    waiting = bool(entries) or body.strip("\n") != ""
    days = re.split(r"^## Version ", rest, maxsplit=1, flags=re.MULTILINE)[0]
    if not waiting and days.strip("\n") == "":
        print(f"changelog.py: nothing since the last release: no entry, nothing under `{UNRELEASED}`, no day section",
              file=sys.stderr)
        return 1
    if waiting:
        code = fold(root, args.date)
        if code != 0:
            return code
    head, body, rest = changelog_parts(root)
    heading = f"## Version {args.version} ({args.date})"
    new = head + "\n" + heading + "\n" + (("\n" + rest) if rest else "")
    write_atomic(root / CHANGELOG, new, crlf(root))
    print(f"Gave the days since the last release the heading `{heading}`.")
    print(f"Commit {CHANGELOG}" + (" and the removals" if entries else "") + ", then tag v" + args.version + ".")
    return 0


def cmd_check_release(args: argparse.Namespace) -> int:
    """A tag's changelog, as release.yml asks before it builds: no entry left in docs/changelog/ (they are folded in
    first, or the plugins' What's new would lack them), and a release's version with its heading."""
    root = args.root
    m = TAG_RE.match(args.tag)
    if not m:
        print(f"changelog.py: {args.tag} is no release tag: vX.Y.Z, or with a pre-release's suffix (v0.1.0-rc.1)",
              file=sys.stderr)
        return 1
    problems = []
    left = entry_files(root)
    if left:
        names = ", ".join(p.relative_to(root).as_posix() for p in left)
        problems.append(f"{len(left)} entr{'y' if len(left) == 1 else 'ies'} left in {ENTRIES_DIR.as_posix()}/ "
                        f"({names}): fold them in first (scripts/changelog.py release)")
    if not (root / CHANGELOG).is_file():
        problems.append(f"no {CHANGELOG}")
    else:
        text = read_changelog(root)
        problems += release_problems(text)
        version = m.group(1)
        if m.group(2) is None and parse_version(version) not in [v for _, _, v, _ in release_headings(text)]:
            problems.append(f"{CHANGELOG} has no `## Version {version} (YYYY-MM-DD)` heading: give the release its "
                            f"heading first (scripts/changelog.py release --version {version} --date YYYY-MM-DD)")
    for p in problems:
        print(f"changelog.py: {args.tag}: {p}", file=sys.stderr)
    if problems:
        return 1
    print(f"changelog.py: {args.tag}: " + ("a pre-release, " if m.group(2) else f"Version {m.group(1)} has its heading, ")
          + f"no entry left in {ENTRIES_DIR.as_posix()}/")
    return 0


# ---- the self-test ------------------------------------------------------------------------------------------------

PREAMBLE = "# Changelog\n\nWhat changed, newest first.\n\n"
OLDER = ("## 2026-01-01\n\nClaude Desktop extension: **0.16.0** (was 0.15.0).\n\n"
         "### Something old ([#1](https://github.com/Owner/Repo/pull/1))\n\n**Added**\n- A thing.\n")
EXISTING = ("Claude Desktop extension: **0.17.0** (was 0.16.0).\n\n### Already here\n\n**Fixed**\n"
            "- A fix already under Unreleased.\n")


def entry_text(title: str, ext: str | None = None, kind: str = "**Added**") -> str:
    lead = f"Claude Desktop extension: {ext}.\n\n" if ext else ""
    item = title.split(" (")[0].lower()
    return f"{lead}### {title}\n\n{kind}\n- An item of {item}, long enough to\n  wrap onto a second line.\n"


def selftest(_: argparse.Namespace) -> int:
    failures: list[str] = []

    def expect(cond: bool, what: str) -> None:
        print(("ok    " if cond else "FAIL  ") + what)
        if not cond:
            failures.append(what)

    def run(root: Path, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, str(SCRIPT_PATH), "--root", str(root), *args],
                              capture_output=True, text=True, check=False)

    env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull, GIT_CONFIG_NOSYSTEM="1")

    def g(root: Path, *args: str, when: int | None = None) -> str:
        e = dict(env)
        if when is not None:
            e["GIT_AUTHOR_DATE"] = e["GIT_COMMITTER_DATE"] = f"@{when} +0000"
        r = subprocess.run(["git", "-C", str(root), "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                            "-c", "commit.gpgsign=false", "-c", "init.defaultBranch=main", *args],
                           capture_output=True, text=True, env=e, check=False)
        if r.returncode != 0:
            raise RuntimeError(f"git {' '.join(args)}: {r.stderr.strip()}")
        return r.stdout

    tmp = Path(tempfile.mkdtemp(prefix="changelog-selftest-"))
    try:
        # ---- check: what is refused ----
        bad = tmp / "bad"
        (bad / ENTRIES_DIR).mkdir(parents=True)
        (bad / CHANGELOG).write_text(PREAMBLE + "## Unreleased\n\n" + OLDER)
        (bad / ENTRIES_DIR / README).write_text("Not an entry.\n")
        good = entry_text("Fine")
        cases = {
            "Bad_Name.md": (good, "not an entry's name"),
            "no-heading.md": ("**Added**\n- A thing.\n", "no `### ` heading"),
            "two-headings.md": (good + "\n### Another\n\n**Fixed**\n- A fix.\n", "a second `### ` heading"),
            "day-heading.md": ("## 2026-01-02\n\n" + good, "a `# ` or `## ` heading"),
            "no-list.md": ("### Title\n\nWords alone.\n", "no **Added**, **Fixed** or **Changed** list"),
            "empty-list.md": ("### Title\n\n**Fixed**\n\n**Added**\n- A thing.\n", "**Fixed** has no `- ` item"),
            "tab.md": ("### Title\n\n**Added**\n-\tA thing.\n", "a tab"),
            "lone-cr.md": ("### Title\r\n\r\n**Added**\r\n- A\rthing.\r\n", "a carriage return alone"),
            "long.md": ("### Title\n\n**Added**\n- " + "x" * 120 + "\n", "122 columns"),
            "no-newline.md": (good.rstrip("\n"), "no newline at the end"),
            "two-newlines.md": (good + "\n", "more than one newline"),
            "bad-extension.md": ("Claude Desktop extension: 0.18.0 (was 0.17.0)\n\n" + good, "the extension's line"),
            "extension-later.md": (good + "Claude Desktop extension: **0.18.0** (was 0.17.0).\n", "goes first"),
            "extension-no-blank.md": ("Claude Desktop extension: **0.18.0** (was 0.17.0).\n" + good, "a blank line"),
        }
        for name, (text, _) in cases.items():
            (bad / ENTRIES_DIR / name).write_text(text)
        (bad / ENTRIES_DIR / "sub").mkdir()
        (bad / ENTRIES_DIR / "sub" / "nested.md").write_text(good)
        r = run(bad, "check")
        expect(r.returncode == 1, "check: exit 1 on bad entries")
        lines = r.stdout.splitlines()
        for name, (_, words) in cases.items():
            expect(any(line.startswith(f"docs/changelog/{name}:") and words in line for line in lines),
                   f"check: {name} refused ({words})")
        expect(any(line.startswith("docs/changelog/sub/nested.md:1:") for line in lines), "check: a subfolder's file")
        expect(not any("README.md" in line for line in lines), "check: README.md is no entry")
        expect(all(re.match(r"^\S+:\d+: ", line) for line in lines), "check: each problem names its file and line")
        r = run(bad, "fold")
        expect(r.returncode == 1 and (bad / CHANGELOG).read_text() == PREAMBLE + "## Unreleased\n\n" + OLDER,
               "fold: refused while an entry is bad, nothing changed")
        (bad / CHANGELOG).write_text(PREAMBLE + OLDER)
        for f in (bad / ENTRIES_DIR).rglob("*.md"):
            if f.name != README:
                f.unlink()
        r = run(bad, "check")
        expect(r.returncode == 1 and "no `## Unreleased` heading" in r.stdout, "check: CHANGELOG.md's Unreleased")

        # ---- a repository: entries merged, committed, tied and not yet committed ----
        repo = tmp / "repo"
        (repo / ENTRIES_DIR).mkdir(parents=True)
        g(repo, "init", "-q")
        g(repo, "remote", "add", "origin", "git@github.com:Owner/Repo.git")
        before = PREAMBLE + "## Unreleased\n\n" + EXISTING + "\n" + OLDER
        (repo / CHANGELOG).write_text(before)
        (repo / ENTRIES_DIR / README).write_text("The entries.\n")
        g(repo, "add", "-A")
        g(repo, "commit", "-q", "-m", "Start", when=100)
        r = run(repo, "check")
        expect(r.returncode == 0, "check: no entries is fine")
        r = run(repo, "fold")
        expect(r.returncode == 0 and (repo / CHANGELOG).read_text() == before, "fold: no entry changes nothing")
        r = run(repo, "show")
        expect(r.stdout == "## Unreleased\n\n" + EXISTING, "show: the Unreleased section as it is")

        g(repo, "checkout", "-q", "-b", "a")
        (repo / ENTRIES_DIR / "alpha.md").write_text(entry_text("Alpha", "**0.18.0** (was 0.17.0)"))
        g(repo, "add", "-A")
        g(repo, "commit", "-q", "-m", "Alpha", when=1000)
        g(repo, "checkout", "-q", "main")
        g(repo, "checkout", "-q", "-b", "b")
        linked = "Beta ([#9](https://github.com/Owner/Repo/pull/9))"
        (repo / ENTRIES_DIR / "beta.md").write_text(entry_text(linked, kind="**Fixed**"))
        g(repo, "add", "-A")
        g(repo, "commit", "-q", "-m", "Beta", when=2000)
        g(repo, "checkout", "-q", "main")
        g(repo, "merge", "-q", "--no-ff", "-m", "Merge pull request #5 from Owner/a", "a", when=1100)
        g(repo, "merge", "-q", "--no-ff", "-m", "Merge pull request #6 from Owner/b", "b", when=2100)
        # c: its entry comes in its merge of main (as an entry moved out of CHANGELOG.md during that merge does)
        g(repo, "checkout", "-q", "-b", "c")
        (repo / "work.txt").write_text("c's work\n")
        g(repo, "add", "-A")
        g(repo, "commit", "-q", "-m", "Work on c", when=2200)
        g(repo, "checkout", "-q", "main")
        (repo / ENTRIES_DIR / "gamma.md").write_text(entry_text("Gamma"))
        (repo / ENTRIES_DIR / "epsilon.md").write_text(entry_text("Epsilon", kind="**Changed**"))
        g(repo, "add", "-A")
        g(repo, "commit", "-q", "-m", "Gamma and epsilon on main", when=3000)
        g(repo, "checkout", "-q", "c")
        g(repo, "merge", "-q", "--no-ff", "--no-commit", "main")
        (repo / ENTRIES_DIR / "theta.md").write_text(entry_text("Theta"))
        g(repo, "add", "-A")
        g(repo, "commit", "-q", "-m", "Merge branch 'main' into c", when=3200)
        g(repo, "checkout", "-q", "main")
        g(repo, "merge", "-q", "--no-ff", "-m", "Merge pull request #7 from Owner/c", "c", when=3500)
        (repo / ENTRIES_DIR / "delta.md").write_text(entry_text("Delta", "**0.19.0** (was 0.18.0)"))
        (repo / ENTRIES_DIR / "zeta.md").write_text(entry_text("Zeta"))
        g(repo, "add", "--", "docs/changelog/zeta.md")   # staged, not committed

        r = run(repo, "check")
        expect(r.returncode == 0, "check: well-formed entries pass")
        order = ["Delta", "Zeta", "Theta", "Epsilon", "Gamma", "Beta", "Alpha"]
        want = ("## Unreleased\n\nClaude Desktop extension: **0.19.0** (was 0.16.0).\n\n"
                + "\n".join(entry_text({"Alpha": "Alpha ([#5](https://github.com/Owner/Repo/pull/5))",
                                        "Theta": "Theta ([#7](https://github.com/Owner/Repo/pull/7))",
                                        "Beta": linked}.get(t, t),
                                       kind={"Beta": "**Fixed**", "Epsilon": "**Changed**"}.get(t, "**Added**"))
                            for t in order)
                + "\n" + EXISTING.split("\n\n", 1)[1])
        r = run(repo, "show")
        expect(r.returncode == 0 and r.stdout == want,
               "show: the folded section (order by merge, links, an entry added in a merge of main, extension line)")
        if r.stdout != want:
            print(r.stdout + r.stderr)
        expect(len(list((repo / ENTRIES_DIR).glob("*.md"))) == 8 and (repo / CHANGELOG).read_text() == before,
               "show: changes nothing")
        r = run(repo, "fold")
        after = (repo / CHANGELOG).read_text()
        expect(r.returncode == 0, "fold: exit 0")
        expect(after == PREAMBLE + want + "\n" + OLDER, "fold: CHANGELOG.md as shown")
        expect(after.startswith(PREAMBLE + "## Unreleased\n") and after.endswith("\n" + OLDER),
               "fold: the preamble and the dated sections untouched byte for byte")
        expect([p.name for p in (repo / ENTRIES_DIR).iterdir()] == [README], "fold: the entry files gone")
        status = g(repo, "status", "--porcelain")
        expect("D  docs/changelog/alpha.md" in status and "D  docs/changelog/gamma.md" in status
               and "D  docs/changelog/theta.md" in status,
               "fold: committed entries removed with git rm")
        expect("zeta.md" not in status, "fold: a staged entry taken out of the index too")
        expect("delta.md" not in status, "fold: an untracked entry unlinked")
        expect("Folded 7 entries" in r.stdout and "Commit CHANGELOG.md" in r.stdout, "fold: says what it did")
        r = run(repo, "fold")
        expect(r.returncode == 0 and (repo / CHANGELOG).read_text() == after, "fold: again changes nothing")

        # ---- cut ----
        r = run(repo, "cut", "--date", "2026-02-30")
        expect(r.returncode == 1 and "not a day" in r.stderr, "cut: refuses a date that is no day")
        r = run(repo, "cut", "--date", "2026-1-5")
        expect(r.returncode == 1, "cut: refuses a date not as YYYY-MM-DD")
        r = run(repo, "cut", "--date", "2026-01-01")
        expect(r.returncode == 1 and "already" in r.stderr, "cut: refuses a day that has its section")
        expect((repo / CHANGELOG).read_text() == after, "cut: a refusal changes nothing")
        (repo / ENTRIES_DIR / "eta.md").write_text(entry_text("Eta", "**0.20.0** (was 0.19.0)"))
        r = run(repo, "cut", "--date", "2026-03-01")
        cut = (repo / CHANGELOG).read_text()
        body = want.split("\n\n", 2)[2]
        expect(r.returncode == 0 and cut == PREAMBLE + "## Unreleased\n\n## 2026-03-01\n\n"
               + "Claude Desktop extension: **0.20.0** (was 0.16.0).\n\n" + entry_text("Eta") + "\n" + body
               + "\n" + OLDER, "cut: folds, then the day's section below an empty Unreleased")
        expect(not (repo / ENTRIES_DIR / "eta.md").exists(), "cut: the entry file gone")
        r = run(repo, "cut", "--date", "2026-03-02")
        expect(r.returncode == 1 and "nothing" in r.stderr and (repo / CHANGELOG).read_text() == cut,
               "cut: refuses an empty Unreleased")
        r = run(repo, "show")
        expect(r.stdout == "## Unreleased\n", "show: an empty Unreleased")
        r = run(repo, "check")
        expect(r.returncode == 0, "check: passes after the cut")

        # ---- no git: by name, unlinked, no links ----
        plain = tmp / "plain"
        (plain / ENTRIES_DIR).mkdir(parents=True)
        (plain / CHANGELOG).write_text(PREAMBLE + "## Unreleased\n")
        (plain / ENTRIES_DIR / "b.md").write_text(entry_text("B"))
        (plain / ENTRIES_DIR / "a.md").write_text(entry_text("A"))
        r = run(plain, "fold")
        expect(r.returncode == 0 and (plain / CHANGELOG).read_text()
               == PREAMBLE + "## Unreleased\n\n" + entry_text("A") + "\n" + entry_text("B"),
               "fold outside git: by name, into an Unreleased at the end of the file")
        expect(not list((plain / ENTRIES_DIR).iterdir()), "fold outside git: the files unlinked")

        # ---- CR LF, as a Windows checkout may hold the files ----
        windows = tmp / "windows"
        (windows / ENTRIES_DIR).mkdir(parents=True)
        (windows / CHANGELOG).write_bytes((PREAMBLE + "## Unreleased\n\n" + EXISTING + "\n" + OLDER)
                                          .replace("\n", "\r\n").encode())
        (windows / ENTRIES_DIR / "a.md").write_bytes(entry_text("A", "**0.18.0** (was 0.17.0)")
                                                     .replace("\n", "\r\n").encode())
        r = run(windows, "check")
        expect(r.returncode == 0, "check: CR LF read as LF")
        r = run(windows, "fold")
        crlf_want = (PREAMBLE + "## Unreleased\n\nClaude Desktop extension: **0.18.0** (was 0.16.0).\n\n"
                     + entry_text("A") + "\n" + EXISTING.split("\n\n", 1)[1] + "\n" + OLDER).replace("\n", "\r\n")
        expect(r.returncode == 0 and (windows / CHANGELOG).read_bytes() == crlf_want.encode(),
               "fold: a CR LF CHANGELOG.md written back with CR LF")

        # ---- releases: their headings kept by fold and cut, checked, made by release, asked for by check-release ----
        rel = tmp / "releases"
        (rel / ENTRIES_DIR).mkdir(parents=True)
        g(rel, "init", "-q")
        released = "## Version 0.1.0 (2026-01-02)\n\n" + OLDER
        start = PREAMBLE + "## Unreleased\n\n" + EXISTING + "\n" + released
        (rel / CHANGELOG).write_text(start)
        (rel / ENTRIES_DIR / README).write_text("The entries.\n")
        g(rel, "add", "-A")
        g(rel, "commit", "-q", "-m", "Start", when=100)
        r = run(rel, "check")
        expect(r.returncode == 0 and "1 release's heading" in r.stdout, "check: a release's heading is well formed")
        r = run(rel, "check-release", "--tag", "v0.1.0")
        expect(r.returncode == 0, "check-release: a release with its heading and no entry left passes")
        r = run(rel, "check-release", "--tag", "v0.2.0")
        expect(r.returncode == 1 and "no `## Version 0.2.0 (YYYY-MM-DD)` heading" in r.stderr,
               "check-release: a release's version with no heading refused")
        r = run(rel, "check-release", "--tag", "v0.2.0-rc.1")
        expect(r.returncode == 0, "check-release: a pre-release needs no heading")
        for tag in ("0.2.0", "v0.2", "v0.2.0.1", "release"):
            r = run(rel, "check-release", "--tag", tag)
            expect(r.returncode == 1 and "no release tag" in r.stderr, f"check-release: {tag} is no tag")
        (rel / ENTRIES_DIR / "late.md").write_text(entry_text("Late"))
        for tag in ("v0.1.0", "v0.2.0-rc.1"):
            r = run(rel, "check-release", "--tag", tag)
            expect(r.returncode == 1 and "1 entry left in docs/changelog/ (docs/changelog/late.md)" in r.stderr,
                   f"check-release: {tag} refused while an entry file is left")
        r = run(rel, "fold")
        folded = (rel / CHANGELOG).read_text()
        expect(r.returncode == 0 and folded.endswith("\n" + released) and "### Late" in folded.split("## Version")[0],
               "fold: under Unreleased, the release's section untouched byte for byte")
        r = run(rel, "cut", "--date", "2026-01-05")
        cutted = (rel / CHANGELOG).read_text()
        expect(r.returncode == 0 and cutted == PREAMBLE + "## Unreleased\n\n## 2026-01-05\n"
               + folded.split("## Unreleased\n", 1)[1].split("## Version", 1)[0] + released,
               "cut: the day's section above the release's heading")

        bad_headings = {
            "## Version 0.2 (2026-01-09)": "not a release's heading",
            "## Version 0.2.0": "not a release's heading",
            "## Version 0.2.0 (2026-02-30)": "not a day",
            "## Version 0.1.0 (2026-01-09)": "Version 0.1.0 again",
        }
        for line, words in bad_headings.items():
            (rel / CHANGELOG).write_text(cutted.replace("## 2026-01-05\n", line + "\n\n## 2026-01-05\n"))
            r = run(rel, "check")
            expect(r.returncode == 1 and words in r.stdout and r.stdout.startswith("CHANGELOG.md:"),
                   f"check: `{line}` refused ({words})")
        (rel / CHANGELOG).write_text(cutted.replace("## 2026-01-05\n", "## Version 0.2.0 (2026-01-05)\n\n## 2026-01-05\n"))
        r = run(rel, "check")
        expect(r.returncode == 0 and "2 releases' headings" in r.stdout, "check: releases newest first pass")
        (rel / CHANGELOG).write_text(cutted.replace("## 2026-01-05\n", "## Version 0.0.5 (2026-01-05)\n\n## 2026-01-05\n")
                                     .replace("## Unreleased\n", "## Version 0.0.6 (2026-01-06)\n\n## Unreleased\n"))
        r = run(rel, "check")
        expect(r.returncode == 1 and "above `## Unreleased`" in r.stdout, "check: a release above Unreleased refused")
        (rel / CHANGELOG).write_text(cutted.replace("## 2026-01-05\n", "## Version 0.0.3 (2026-01-05)\n\n## 2026-01-05\n"))
        r = run(rel, "check")
        expect(r.returncode == 1 and "the newest release first" in r.stdout, "check: releases out of order refused")
        (rel / CHANGELOG).write_text(cutted)

        # release: refusals change nothing
        for args, words in ((("--version", "0.2", "--date", "2026-01-09"), "no release's version"),
                            (("--version", "0.2.0-rc.1", "--date", "2026-01-09"), "no release's version"),
                            (("--version", "0.2.0", "--date", "2026-1-9"), "no day"),
                            (("--version", "0.1.0", "--date", "2026-01-09"), "already"),
                            (("--version", "0.0.9", "--date", "2026-01-09"), "below the newest release")):
            r = run(rel, "release", *args)
            expect(r.returncode == 1 and words in r.stderr and (rel / CHANGELOG).read_text() == cutted,
                   f"release {' '.join(args)}: refused ({words}), nothing changed")
        # a day cut already, nothing waiting: the heading over it
        r = run(rel, "release", "--version", "0.2.0", "--date", "2026-01-09")
        rel1 = (rel / CHANGELOG).read_text()
        expect(r.returncode == 0 and rel1 == cutted.replace("## Unreleased\n\n## 2026-01-05\n",
                                                            "## Unreleased\n\n## Version 0.2.0 (2026-01-09)\n\n"
                                                            "## 2026-01-05\n"),
               "release: the heading over the days cut since the last release, Unreleased empty on top")
        r = run(rel, "check")
        expect(r.returncode == 0 and "2 releases' headings" in r.stdout, "check: passes after the release")
        r = run(rel, "release", "--version", "0.3.0", "--date", "2026-01-10")
        expect(r.returncode == 1 and "nothing since the last release" in r.stderr and (rel / CHANGELOG).read_text() == rel1,
               "release: refused with nothing since the last release")
        # entries waiting: folded, cut into the day, then the heading
        (rel / ENTRIES_DIR / "next.md").write_text(entry_text("Next"))
        r = run(rel, "release", "--version", "0.3.0", "--date", "2026-01-12")
        rel2 = (rel / CHANGELOG).read_text()
        expect(r.returncode == 0 and rel2 == rel1.replace("## Unreleased\n\n", "## Unreleased\n\n## Version 0.3.0 "
                                                          "(2026-01-12)\n\n## 2026-01-12\n\n" + entry_text("Next") + "\n"),
               "release: the entries folded, cut into the day, the day under the version's heading")
        expect(not (rel / ENTRIES_DIR / "next.md").exists(), "release: the entry file gone")
        r = run(rel, "check-release", "--tag", "v0.3.0")
        expect(r.returncode == 0, "check-release: passes after release")
        r = run(rel, "show")
        expect(r.stdout == "## Unreleased\n", "show: an empty Unreleased over the releases")
    except RuntimeError as e:
        failures.append(str(e))
        print(f"FAIL  {e}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    expect(not tmp.exists(), "the temporary folder removed")
    print(f"\nchangelog.py selftest: {'all passed' if not failures else f'{len(failures)} failed'}")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(prog="changelog.py", description=__doc__.strip(),
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=PROJECT_ROOT,
                        help="the repository's root (default: this script's; the self-test points it elsewhere)")
    subs = parser.add_subparsers(dest="command", required=True)

    p = subs.add_parser("check", help="check the entry files and CHANGELOG.md's Unreleased heading")
    p.set_defaults(func=cmd_check)
    p = subs.add_parser("show", help="print the Unreleased section as folding would make it")
    p.set_defaults(func=cmd_show)
    p = subs.add_parser("fold", help="fold the entries into CHANGELOG.md's Unreleased section and remove them")
    p.set_defaults(func=cmd_fold)
    p = subs.add_parser("cut", help="fold, then make the Unreleased section the day's")
    p.add_argument("--date", required=True, help="the day, YYYY-MM-DD")
    p.set_defaults(func=cmd_cut)
    p = subs.add_parser("release", help="cut, then give the days since the last release the version's heading")
    p.add_argument("--version", required=True, help="the release's version, X.Y.Z")
    p.add_argument("--date", required=True, help="its day, YYYY-MM-DD")
    p.set_defaults(func=cmd_release)
    p = subs.add_parser("check-release", help="refuse a tag whose version has no heading, or with entries left")
    p.add_argument("--tag", required=True, help="the tag, vX.Y.Z (or vX.Y.Z-rc.1)")
    p.set_defaults(func=cmd_check_release)
    p = subs.add_parser("selftest", help="run this script's tests in a temporary repository")
    p.set_defaults(func=selftest)

    args = parser.parse_args()
    args.root = args.root.resolve()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
