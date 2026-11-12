# 02. The Projects card: click outside, sorting, bulk export

Planning-document items: "Project/preset popup: close when the user clicks outside (same functionality as
cancel)", "sorting Projects by Last Modified/Created, Size (disk usage), and Name", "Add bulk Project exports".
Size M: three parts that can be three pull requests. Needs nothing (the lists' scrolling is package 01).

## 1. What exists

- The Projects card is `Source/editor/ProjectBrowser.h/.cpp`, a modal card (`kBoxW = 860`, `kBoxH = 640`) the
  Panel routes every event to while it is open (`Panel::pointerDown`, `dialogOpen()`). Its buttons: Delete,
  Import..., Export..., A/B... at the left; Close, Open, Save at the right. There is no Cancel: Close and Escape
  call `ProjectBrowser::close()`.
- **A press outside the card does nothing today** (`ProjectBrowser::pointerDown` has no test of `at.box`). The
  preset picker already closes on one (`PresetPicker::pointerDown`: `if (!at.box.contains(p)) { close(); ... }`),
  and the Panel swallows that press and the second press of a double-click (`closingPress_`). The Settings card
  closes on an outside press too.
- **The order** is `projectstore::View::byDate` (`Source/plugin/ProjectStore.h`), a bool, kept in the folder's
  `.projects-view.json` as `"sort": "saved" | "name"`, mirrored in `EditorPort::ProjectsView`, applied only in
  `projectgroups::arrange` (`Source/editor/ProjectGroups.cpp`). The explorer's PROJECTS forces newest first
  (`Panel.cpp`, `o.byDate = true`).
- **What a listing knows.** `projectstore::Entry` has `name`, `savedMs` (the newest version's), `latest`,
  `versions`, `presets`, `category`, `tags`, `description`. A project has no created time (only each version's
  `saved`, and only the newest 100 versions are kept). The file's size and time are in the cache's `Cached`
  (`ProjectStore.cpp`) but reach neither `Entry` nor `EditorPort::ProjectEntry`.
- **Export** is one project at a time: `ExportCard` holds one name; `Panel::afterExport` asks the host's chooser
  for one save path; `Processor::exportProject` calls `projectstore::load` on the whole file **on the message
  thread** (against CLAUDE.md's rule). The card's own Export uses `chosenName_`; the context menu's uses only the
  first of the chosen (`names[0]`) though several can be chosen (`selected_`, Cmd-click and Shift-click).
  Import already takes many files (`FileRequest::Mode::openMany`, `importFiles`).
- FunkGui's file chooser has no folder mode (`FileRequest::Mode { open, openMany, save }`).

## 2. Part A: a click outside closes

- `ProjectBrowser::pointerDown`: a press outside `at.box` is `close()`, exactly what the Close button does (the
  view kept, a confirmation it was asking for withdrawn, nothing saved or opened). Only a press: a drag that began
  inside and ends outside (projects dragged onto a group) closes nothing.
- `ExportCard`: the same, with its own Done.
- `Panel::pointerDown`: both go through the path the preset picker uses, so the press is dropped and
  `closingPress_` swallows a double-click's second press (else it would land on what is under the card).
- Not the A/B card (assumption A1): its Close ends the listening and puts back what played before.
- A native menu open over the card takes the press itself; nothing to do.

Tests: `gui.projects` gains OUTSIDE (a press outside closes; the focus returns as after Close; a press on the
card's edge does not; a drag out does not; the press reaches nothing behind; with "Delete?" asked, an outside
press closes and deletes nothing). `gui.dump` PROJECTS: EXPORT likewise.

## 3. Part B: sorting

**Keys:** Last modified (the newest version's save: today's "By last saved"), Date created, Size, Name; each can
be reversed. Default: Name, as now.

- **Created.** A new optional field at the project file's root, `"created"` (ms since 1970). Every write of a
  file that lacks it sets it to the oldest kept version's `saved` (so it is fixed from then on, whatever versions
  are removed later); a new project's is its first save. A file not yet rewritten is listed with the same value
  worked out on read, nothing written. An older build reads past the field.
- **Size.** The file's bytes, from `Cached::size`: no new read.
- `projectstore::Entry` and `EditorPort::ProjectEntry` gain `createdMs` and `bytes`; `cachedOf` and
  `Processor::projectsPolled` carry them. A factory project's are 0 and its set's date; it sorts inside FACTORY.
- `projectstore::View`: `byDate` becomes `enum class Order { name, saved, created, size }` and `bool reversed`.
  The file keeps `"sort"` with the two new words, and gains `"reverse": true` only when set. An older build reads
  anything but `"saved"` as by name: fine, and said in ProjectStore.h. `EditorPort::ProjectsView` mirrors both.
- `projectgroups::arrange`: one comparator a key, ties by lower-case own name; newest first, largest first, A to Z
  unless reversed. Groups stay as they are (FAVOURITES, FACTORY, each category, UNCATEGORISED): the order is
  within a group.
- The card's order dropdown (`showMenu(Menu::sort)`, `menuChosen`): four `dropdown::choice` rows and, under a
  separator, a checked "Reversed". The trigger's label: "Last modified", "Date created", "Size", "Name" (an arrow
  when reversed). A row's right-hand column shows what it is sorted by: the date, or "1.2 MB" for Size.
- The explorer keeps newest first (`Order::saved`, not reversed).

Tests: `plugin.projects` VIEW (the four words round-trip; an old file's `"saved"` and `"name"` read as before; a
file with `"size"` read by the old rule is by name), CACHED (`bytes` and `createdMs` come without a parse:
`projectstore::parses` does not move), SAVE (`created` set once and kept after the oldest version is removed).
`gui.projects` SORT: each key, reversed, ties, the label, the row's column; the menu's PNG in `gui.menus`.

## 4. Part C: bulk export

**What the user does:** choose several projects (Cmd-click, Shift-click), press Export... (or the context menu's
Export): the Export card says "Export 12 projects", offers the same two formats (Source .zip, Package .fmpack) and
"all versions", and asks for one file to save. The result is one `.zip`, the bundle, with one export file per
project in it. Import... takes a bundle and brings every project in.

- **The bundle** (`Source/plugin/ProjectExport.h/.cpp`): a zip with `bundle.json`
  (`{"format": "funk-playground-bundle", "version": 1, "plugin", "kind", "projects": [{"name", "file"}]}`) and each
  project's own export under its own name (`<Name>.fmpack` or `<Name>.zip`), names made unique case aside (the
  default APFS is case-insensitive). Unzipped by hand it is a folder of ordinary exports, each importable alone.
  A factory project goes under its own name (`ownName`), as a single export does. Ids are left out, as exports
  leave them out.
- `ProjectExport::read` recognises a bundle and gives its projects; `importProject`/`importFiles` loop over them
  (the other kind's projects refused one by one, the rest taken, the outcome counted in words).
- **Off the message thread.** A job on a thread of the processor's own, shaped as the saver is (`takeSave`,
  `runSave`, `landSave`): `EditorPort::startExport(names, package, allVersions, path)`, `exporting()` giving
  `{done, total, failed, finished}`, `cancelExport()`. It reads each file with `projectstore::load` on that
  thread, writes the bundle through a temporary beside the target and renames it into place, and never replaces a
  file that appeared meanwhile. **A single export goes through the same job**, which removes today's whole-file
  read on the message thread. A processor that goes joins the job (cancelled).
- **The card.** `ExportCard` gains a bulk mode: no info fields (they are one project's), the count, the first
  names and "and 8 more", the format, "all versions", the two Export buttons, Done. While it runs: "EXPORTING 3 OF 12" in the
  status and Cancel. After: "EXPORTED 12 PROJECTS" or what failed, by name.
- `ProjectBrowser`: Export... is bulk when more than one project is chosen; the context menu's Export takes every
  chosen name.
- Limits, refused in words before anything is written: 256 projects, 2 GB of project files in all.

Tests: `plugin.export` BUNDLE (N projects out and in again: names, versions, presets, samples the same; unique
names for two that differ by case; a factory project's own name; the other kind's refused inside a bundle, the
rest taken; a cancel leaves no file; the target never replaced; a single export through the job gives the bytes it
gave before). `plugin.projects` ASYNC-style rows: no parse on the message thread during an export
(`projectstore::parses` counted there). `gui.projects` EXPORT: the bulk card, its progress, its PNG in both
themes.

## 5. Docs

README, "The editor" (the Projects card: outside click, the four orders, exporting several) and the export
formats' section (the bundle). CHANGELOG: Added (sorting, bulk export), Changed (a click outside closes).
CLAUDE.md: the project file's `created`; the view's order; the export job (and that no export reads a file on the
message thread any more); the bundle's format beside the two it has.
