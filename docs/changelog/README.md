# The changelog's entries

A pull request that changes what a user of the plugins can see adds one file here, `<name>.md` (lower-case letters,
digits and hyphens), and never edits `CHANGELOG.md`: every pull request used to add its entry at the top of
`CHANGELOG.md`'s Unreleased section, so each merge made every other open one conflict there.

The file holds what would go under `## Unreleased`: a `### Title` heading, then any of the **Added**, **Fixed** and
**Changed** lists, each of `- ` items, lines wrapped at 120 columns. A pull request that raises the Claude Desktop
extension's version starts it with `Claude Desktop extension: **0.18.0** (was 0.17.0).` and a blank line. Leave the
pull request's link out: the fold adds it from the merge.

`scripts/changelog.py check` checks the files (the test `lint.changelog` too), `show` prints the Unreleased section
they would make, and on `main` `fold` moves them into `CHANGELOG.md` and removes them; `cut --date YYYY-MM-DD` folds
and makes the day's section. This file is never an entry.

A release gives the days since the one before a heading of their own, `## Version 0.2.0 (2026-11-20)`, under
`## Unreleased`: `scripts/changelog.py release --version 0.2.0 --date 2026-11-20` on `main` folds what waits here, cuts
the day's section and adds the heading, before the tag. The plugins compile `CHANGELOG.md` in and show their version's
section as What's new, so the release workflow refuses a tag whose version has no heading, or any tag while an entry
waits here (`scripts/changelog.py check-release --tag v0.2.0`).

fold's order: the entries not yet committed come first, by name; then the others, newest first by the time of the
commit that brought each file onto the current branch (along its first-parent line: a pull request's merge, or a
commit made on the branch itself; the newest such commit when a file was removed and added again), ties by name.
