# 03. A screen that shows the differences between versions

Planning-document item: "Enhance the Project versioning GUI: add a screen that shows the diffs between versions."
Size M. Needs 01 (its `Scroll`); gives 04 and 05 the reader they use.

## 1. What the user gets

In the Projects card, with a version chosen: **Compare...** opens a card that shows what changed between two
versions of the project, or between a version and what is in the editor now: the files line by line, the dials,
the samples. Nothing plays and nothing is opened until the user asks.

## 2. What exists

- The A/B card already draws exactly this diff (`Source/editor/AbCard.h/.cpp`): `AbCard::diffOf(a, b)` is a pure
  function from two `Sources` to `vector<FileDiff>` over `textdiff::diff` (`Source/dsp/TextDiff.h`), three lines of
  context, long runs folded into "N lines" gaps, at most 6,000 rows; a pill a file with "+12 -3", "new", "gone";
  line numbers for both sides; removed lines tinted with `danger`, added with `accent`.
- It is tied to listening: its two sides come only from `EditorPort::ab()` (`AbState.sides[].files`), which exist
  only while the processor plays an A/B; the card closes itself when the A/B ends.
- **No call gives the editor one version's files.** `Processor::versions()` and the A/B's start call
  `projectstore::load` on the whole file on the message thread. The cache serves versions' numbers, times and
  notes only (`detailCached`, `EditorPort::projectPolled`).
- The store's `Reader` (one thread a process) reads what the cache lacks and bumps a revision the editor polls
  (`EditorPort::projectsRead`).

## 3. Design

### 3.1 The reader: one version's content, off the message thread

- `projectstore`: a second kind of job for the `Reader`, "a version's content": the file parsed on its thread, one
  version's `files`, `dials` (with their labels), sample names and hashes (never their bytes), `note`, `savedMs`
  kept in a small table (the newest 6 asked, keyed by the file's name, its time and size, and the number), the
  revision bumped under the cache's lock as for a listing.
- `EditorPort`:
  ```cpp
  struct VersionContent { bool read = false; bool missing = false; Sources files; std::vector<ProjectDial> dials;
                          std::vector<std::string> samples; std::string note, when; int number = 0; };
  virtual VersionContent versionPolled(const std::string& project, int number) = 0;   // never parses: asks the Reader
  ```
  `number` 0 is the newest. A factory project's comes from the factory snapshot at once.
- "The editor" as a side is `EditorPort::draft()`'s files and the dials as they are: no read.
- The A/B card's own version list moves to `projectPolled` and `versionPolled` in the same change, which ends its
  whole-file read on the message thread (finding 8 in the README).

### 3.2 `DiffView`: the drawing, taken out of the A/B card

`Source/editor/DiffView.h/.cpp`: `FileDiff`, `diffOf`, `tabLabel`, the file pills, the line rows, the gaps, the
scroll (a `ui::Scroll`, package 01), its keys (PageUp, PageDown, Home, End) and its accessibility, moved from
`AbCard` unchanged in look. Added:

- **Next and previous change** (`]` and `[`, and two small buttons): scrolls to the next run of changed rows.
- **A netlist's drawing lines.** Rows that differ only in `*@` lines are folded into one row, "the drawing
  changed" (`cir::stripLayout` for the comparison): they are layout, not the program.
- **Two more pills after the files' when they differ:** DIALS (a row a dial that differs: its name, its value
  each side as the panel says it, `dialText`; a dial only one side has, said so) and SAMPLES (added, removed,
  replaced, by name).

`AbCard` uses `DiffView` and draws as before; `gui.ab` must pass unchanged.

### 3.3 `DiffCard`: the screen

`Source/editor/DiffCard.h/.cpp`, a card as the others (routed by the Panel while open, Escape and a click outside
close it, package 02's rule):

- Header: `[v12  ▾]  →  [v15  ▾]`, two dropdowns (`editor/Dropdown.h`) listing "Editor" and every version (its
  number, its time, its note cut to fit); under each its time and note in full. **Swap** between them.
- A line of what differs: "3 files, 2 dials, 1 sample", or "The same".
- The `DiffView`.
- Buttons: **Open v12**, **Open v15** (through the Projects card's own open, with its "unsaved changes" question),
  **A/B...** (closes this card and starts the A/B card with these two sides), **Close**.
- While a side is being read: "Reading version 12..." in the view's place; the card polls `projectsRead()` each
  frame, as `ProjectBrowser::poll` does.

### 3.4 Ways in

- Projects card, the versions list: a **Compare...** button beside A/B..., enabled with a project chosen. With one
  version chosen it compares the version before it with it (the first version: with the editor); with the project
  open and edited, "Editor" is offered as the right side first.
- The versions list's context menu: "Compare with the version before", "Compare with the editor".
- The top bar's project pill menu: "Compare with the saved version" when the editor has unsaved changes.

## 4. Work

1. The Reader's job, `versionPolled`, the Processor's side; the A/B card's list on it.
2. `DiffView` out of `AbCard` (a move, then the three additions).
3. `DiffCard`, the Panel's routing (`dialogOpen`, pointer, wheel, keys, accessibility ids of its own), the ways
   in, `ProjectBrowser`'s button and menu rows.
4. Docs and probes.

## 5. Tests

- `plugin.projects` gains VERSION: a version's content arrives through the Reader, with `projectstore::parses`
  unchanged on the message thread; the table keeps the newest 6; a file rewritten by another process is read
  again; a number that does not exist is `missing`; a factory project's at once.
- New probe `gui.diff`: OPEN (from the button, the menus, the pill), PICK (both dropdowns, Swap, "Editor"), DIFF
  (a planted edit, an added file, a removed one: the pills' labels, the rows, the gaps), DRAWING (a netlist redrawn
  only: one folded row), DIALS, SAMPLES, NEXT (the change keys), SCROLL, KEYS, A11Y, OPENS (Open asks when the
  editor has changes), AB (hands both sides to the A/B card), READING (the placeholder, then the diff with no
  parse on the message thread). PNGs in both themes, the least window and a wide one.
- `gui.ab` unchanged and green: the proof the move changed nothing.

## 6. Docs

README, "The editor": the Projects card's Compare. CHANGELOG: Added. CLAUDE.md: `DiffView` and `DiffCard` in the
`Source/editor/**` row; the Reader's second job and `versionPolled` beside `projectPolled`; the sentence "The A/B
card's versions are still read whole" removed.

## 7. Notes

- Side by side instead of one column was considered: the card is 860 px wide and code lines run to 120 columns, so
  one column with both line numbers (the A/B card's look) stays.
- A diff of two long files is worked out on the message thread today (`textdiff::diff` with a cap of 4,000). If
  `gui.diff` shows a frame over 10 ms for the largest factory project, move `diffOf` to the Reader's thread with
  the content; the card already has a "reading" state to show meanwhile.
