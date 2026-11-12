# Feature plan, November 2026

Status: researched and planned, 2026-11-12. Nothing here is built yet. The source is the planning document
"TestingPlayground Planning Document" (Projects, Circuit Editor, Code Editor, Explorer, General, GUI Panel, FunkGui).
Each item is a work package in a file of its own, written so that one implementing session can take it with this
README and CLAUDE.md and nothing else.

Researched against: TestingPlayground `a759725`, FunkGui `1b624d1` (v0.26.1, the pin), FMinus `0a1d8c2` (the pin is
`3ed1ba6`: check anything FMinus-related at the pin), the website repository (TestingPlaygroundBackend) `d8741a5`.
File and line references are as of those commits; a function's name is the reference to trust, its line a hint.
Nothing was built or run for this plan (the session had no toolchain): every "today it does X" comes from reading
the code.

## 1. What the research found that changes the brief

1. **Find-and-replace already exists.** Cmd-Alt-F (or the chevron on the Cmd-F bar) gives replace one, replace all,
   match case and whole word (`Source/editor/CodeAssist.cpp`: `openFind`, `replaceOne`, `replaceAll`; README, "The
   editor"). The item becomes: make it findable, add search across all files, and give the AI the same (package 06).
2. **The lists scroll the wrong way on a Mac, not only at another speed.** FunkGui's `TextEditor` ignores
   `WheelEvent::reversed` (JUCE's deltas already carry the system's direction); the explorer, the Projects card,
   the Settings card, a `.gui` panel and the schematic's pan flip the delta when `reversed` is set. With natural
   scrolling on (macOS's default) they therefore move opposite to the code editor. They also step in whole rows
   or at another rate, and none has a draggable thumb (package 01). Confirmed from JUCE 8.0.4's source on
   2026-11-12 (package 01, section 2); still to be felt on a Mac.
3. **FunkGui has no middle button, no pinch and no 3D.** `PointerEvent` carries no button (a middle press arrives as
   a left one), there is no magnify event, and the GPU layer has no depth buffer, mesh, camera or offscreen pass but
   the picture read-back. Blender-style navigation, 3D and post-effects each need a FunkGui release first
   (package 10), then a pin move here.
4. **FunkGui's GPU layer needs JUCE**, so "examples without JUCE" needs the renderer split from its JUCE host and a
   new host (package 10, F2).
5. **In a DAW the plugin has no settings at all.** The Settings card is the Standalone's devices card. Updates,
   the changelog, the debug page and the usage page all need a Settings card every host has (package 07). The
   factory-projects spec (website repo, `docs/factory-projects.md`, section 7) already expects one for its switch.
6. **The chat keeps almost nothing of its usage.** One display string for the last reply; retries, the planner's
   requests and the model that actually served a request are dropped; OpenRouter's reported cost and price list are
   read past (package 09).
7. **No "latest version" address exists.** Releases go to GitHub and are copied to the website's download page;
   the site's `release.json` is compiled into its pages, not served (package 08).
8. **Exports and the A/B card read a project's file whole on the message thread**, against the rule in CLAUDE.md.
   Packages 02 and 03 fix both on the way.

## 2. Decisions

Decided with the user on 2026-11-12:

| | Question | Decision |
|---|---|---|
| D1 | How git push/pull works | **The user's own git, one repository per project.** The git on the machine, its credentials; a plain-text working folder per linked project; a commit per saved version. No libgit2, no GitHub API |
| D2 | What "mouse events on GUI components" means | **The pointer drives params, and the pointer goes into shaders.** Not declarative click actions, not a new call into the FMinus program |
| D3 | 3D content | **Built-in lit shapes, imported OBJ models, meshes from audio data.** No glTF, no PBR |
| D4 | Find-and-replace | **Make it discoverable; search and replace across all files; give the AI replace-all and grep options.** No regex captures, no preserve-case |
| D5 | Where the plugin learns of a new version | **The website**: one JSON at testingplayground.seanfunk.xyz, the host the factory projects use |
| D6 | How the update check is switched on | **Asked once, with the factory offer.** Nothing is fetched before a yes |
| D7 | What "What's new" shows | **CHANGELOG.md, compiled in, by release** |
| D8 | What JUCE-free FunkGui examples run on | **A GLFW desktop host** |
| D9 | The 16-file limit | **Stays 16.** Only the tab strip changes |
| D10 | Dollar figures on the usage page | **Exact where the provider reports cost; else a built-in price table, refreshed from the website's JSON**, labelled an estimate with its date |

Assumed by the plan, not asked. Each is cheap to change before its package starts; say so if one is wrong:

| | Assumption | Package |
|---|---|---|
| A1 | A click outside closes the Projects card (its Close) and the Export card (its Done). The A/B card stays as it is: closing it ends a listening session | 02 |
| A2 | Sort keys: Last modified, Date created, Size, Name, each reversible. The explorer's PROJECTS stays newest first | 02 |
| A3 | A bulk export is one `.zip` holding one export file per project, chosen with the ordinary Save chooser; Import reads it back | 02 |
| A4 | A preview is drawn from the project's files when its row is chosen and kept in memory; nothing is added to the project file | 04 |
| A5 | A new version is announced by a pill in the top bar and a status notice, never by a card that opens by itself | 08 |
| A6 | "What's new" shows once per version per machine, shared by both products, the first time an editor opens on that version | 08 |
| A7 | The text size applies to the code tabs and the IR view; completion, hover and signature follow it; the chat does not | 06 |
| A8 | Blender's navigation is added; today's Alt-drag and right-drag pans stay as extras; the arrows keep nudging the selection | 11 |
| A9 | Post-effects apply to a `.gui` panel or a container in it, never to the editor's own chrome | 13 |
| A10 | An OBJ model is a text file of the program (a panel file, 256 KB at most), not a binary asset | 13 |
| A11 | `hover=` writes only a panel-only param (slots 33 to 64); a host parameter is never written by hovering. Shaders get hover as an input | 12 |
| A12 | Git never runs by itself: only on Connect, Push, Pull, Check and Clone | 05 |

## 3. The packages

Size: S is one session, M two or three, L several (and its file says where to cut it).

| # | Package | Planning-document items | Size | Needs |
|---|---|---|---|---|
| [01](01-scrolling.md) | One scrolling behaviour, the code editor's | Explorer: scrolling; Projects popup: scrolling | M | none |
| [02](02-projects-card.md) | Projects card: click outside, sorting, bulk export | Popup: close on outside click, sorting; bulk exports | M | none (01 for the lists) |
| [03](03-version-diff.md) | Version diff screen | Versioning GUI: diffs between versions | M | 01 |
| [04](04-panel-previews.md) | Panel previews in the Projects card | Panel previews | M to L | 03 (its reader) |
| [05](05-git.md) | Git push and pull | Git integration | L | 03 (to compare), 07 (its page) |
| [06](06-code-editor.md) | Code editor: find in files, the AI's grep, tabs, text size | Find-and-replace; file tabs; text size | L, four parts | none |
| [07](07-settings-and-debug.md) | Settings for every host, with pages; the Debug page | Settings pages; debug page | M | none (F1 for the GPU's rows) |
| [08](08-updates-and-whats-new.md) | The offer, the update check, What's new | Detect updates; changelog | L | 07, the website's endpoint |
| [09](09-usage.md) | AI usage and cost page | Model and token usage, bar charts, prices | M to L | 07 (08 for the refresh) |
| [10](10-funkgui.md) | FunkGui: input, renderer split and GLFW host, 3D, effect layers, examples | FunkGui: 3D, post-effects, examples | L, five releases | none |
| [11](11-circuit-navigation.md) | Circuit editor navigation as Blender's Node Editor | Circuit Editor | M | 10 (F1) |
| [12](12-panel-pointer.md) | `.gui`: the pointer drives params and shaders | GUI Panel: mouse events | M to L | none |
| [13](13-panel-3d-and-effects.md) | `.gui`: 3D scenes and post-effects | GUI Panel: 3D | L | 10 (F3, F4), 12 |

## 4. Order of work

Four waves here, and FunkGui's releases beside them. Within a wave the packages do not touch the same code, so
they can run in parallel sessions; `Panel.cpp` is the one file nearly all of them edit, so merge often.

1. **Wave 1, no dependencies:** 01 scrolling; 02 Projects card; 06 part D (text size) and part A (find in files).
2. **Wave 2:** 03 version diff; 06 parts B (the AI's tools) and C (tabs); 07 Settings; 12 panel pointer.
3. **Wave 3:** 04 previews; 08 updates and What's new (the website's endpoint first); 09 usage.
4. **Wave 4:** 05 git.

**FunkGui, in parallel from the start** (package 10): F1 input and debug info, then 11 here; F3 3D scenes and F4
effect layers, then 13 here; F2 the renderer split and GLFW host; F5 the examples. Each release ends with the
lead's tag; each use here starts with a pin move (`cmake/Deps.cmake`: tag, commit and version together).

**Other repositories:** the website gains one endpoint (package 08, section 4). FMinus gains nothing unless the
Debug page's version rows are not reachable at the pin (package 07, section 5).

## 5. Rules every package keeps

These are CLAUDE.md's, gathered for an implementer; CLAUDE.md wins where they differ.

- **Done** means `cmake --workflow --preset dev-verify` exits 0 with no warning from our sources, for both
  products. Shared code is both products': run both, and look at both products' PNGs
  (`build-dev/probe-out/<key>/`) after any change to the editor.
- **Probes** are spec rows, no golden files, one file a probe with its `// PROBE` first line, run through ctest
  only. They never reach the network (a scripted transport), never touch the user's folders (their sandbox), never
  launch an app. A row a machine cannot judge is skipped with `probe::skip`, never left out.
- **Threads.** The audio thread gains no lock, allocation or log. The message thread never reads a project's file
  whole. Anything that draws with bgfx draws on the message thread.
- **The editor draws only what changed.** A new item's motion is `settled()` false; what its `draw()` reads that
  changes with no input goes into its `drawKey()`; a callback the host runs later goes through the Panel's host.
- **Menus and dropdowns** go through `editor/Dropdown.h` and FunkGui's menus. A host change in FunkGui's
  `EditorHost` is made by hand in `Source/editor/nogpu/SoftEditorHost` too.
- **Words.** "this Mac" and "Cmd-" in user-facing text go through `kThisMachine` and `shortcutText`.
- **Formats only gain optional fields**: the project file, the session, the projects' view file, `chat.json`.
- **What Claude sees.** A tool changed means its `kFaces` line, the guide's notes, `FP_MCP_VERSION` raised, and
  the tools/list reply still at most 24,000 bytes for each product (`plugin.mcp` prints the slack: about 600 bytes
  for the synth today).
- **Docs with the code:** README.md for users, CHANGELOG.md under Unreleased, CLAUDE.md for what a session must
  know (a new file's row in "What goes where", a new rule where one is made), the prompt and the guide where the
  AI is taught.
- **Git:** a branch a package, commits that say what changed and why, a pull request only when the user asks.

## 6. Deliberately left out

- glTF and physically based materials; binary model assets (D3, A10).
- Regex captures in replacements, preserve-case, find in selection (D4). The regex engine (`dsp/Regex.h`) has no
  captures by design.
- Raising the 16-file limit (D9).
- libgit2, and any git hosting API (D1). Textual three-way merges inside the plugin: a diverged project is settled
  by keeping one side as the newest version and the other as a version before it (package 05).
- Self-updating. The plugin says a version is out and opens the download page; the installer does the rest.
- Events delivered into the FMinus program, and click actions in `.gui` (D2).
- Post-effects over the editor's own chrome (A9).
