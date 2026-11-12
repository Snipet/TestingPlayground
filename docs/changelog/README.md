# The changelog's entries

A pull request that changes what a user of the plugins can see adds one file here, `<name>.md` (lower-case letters,
digits and hyphens), and never edits `CHANGELOG.md`: every pull request used to add its entry at the top of
`CHANGELOG.md`'s Unreleased section, so each merge made every other open one conflict there.

The file holds what would go under `## Unreleased`: a `### Title` heading, then any of the **Added**, **Fixed** and
**Changed** lists, each of `- ` items, lines wrapped at 120 columns. A pull request that raises the Claude Desktop
extension's version starts it with `Claude Desktop extension: **0.18.0** (was 0.17.0).` and a blank line. Leave the
pull request's link out: the fold adds it from the merge.

`scripts/changelog.py check` checks the files (the test `lint.changelog` too), `show` prints the Unreleased section
they would make, and on `main` `fold` moves them into `CHANGELOG.md`, newest first, and removes them; `cut --date
YYYY-MM-DD` folds and makes the day's section. This file is never an entry.
