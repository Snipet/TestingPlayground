# 07. Settings for every host, with pages; the Debug page

Planning-document items: "In the settings menu, add pages to view different categorized settings" and "Add a debug
page that shows debug information from FunkGui and FMinus". Size M. Needs nothing; the GPU's rows are fuller after
FunkGui's F1 (package 10). Packages 05, 06, 08 and 09 each put a page or a row here.

## 1. What exists

- `Source/editor/SettingsCard.h/.cpp` is the **Standalone's devices card** and nothing else: OUTPUT, INPUT (the
  effect), AUDIO, MIDI (the synth), Done. One flat list of controls and lines (`Places{controls, lines}`, `Act`),
  no pages. It opens from the Standalone's Settings... (Cmd-,: `EditorPort::settingsAsked`, a counter the Panel
  polls) and from the effect's muted-input notice. `settings_.open()` returns false with no devices.
- **In a host (AU, VST3) there is no settings UI**: `EditorPort::devices()` is null and `settingsAsked()` never
  moves.
- The other preferences are scattered controls: the theme (the top bar's button) and the zoom (the status bar's
  pill), both in UiPreferences; Format on Run (the code's status line, UiPreferences `codeFormatOnRun`); the
  chat's provider, key and models (the key dialog, `chat.json`); the factory answer (`factory.json`, no UI yet).
- UiPreferences (FunkGui) is one file a product, ints only, message thread only; a change bumps `revision()`,
  which every EditorHost in the process watches.
- Debug information that already exists but shows nowhere:
  - FunkGui: `EditorHost::diagnostics()` (`frames`, `overflows`, `attachFailures`, `displayLinked`, `fps`,
    `scale`, `zoomPercent`, `skipped`, `unchanged`), `surfaceAttached()`, `showingFallback()`; `BgfxSink`'s
    `lastVertexCount()`, `lastDrawCalls()`, `lastUploadBytes()`, `overflowCount()`, `imageTextureCount()`;
    `BgfxContext`'s `transientVbBytes()`, `instancing()`; `FramePump`'s `measuredFps()`, `isDisplayLinked()`,
    `framesRun()`; `shaders::target()` (platform and profile); `gpuPicturesSupported()`. No renderer or GPU name,
    no version function (the version is the generated `product::kFunkGuiVersion`). The Panel holds only a
    `HostServices*`.
  - FMinus: `fminus/FMinus.h` exports `fminus/Config.h`'s `FMINUS_VERSION_STRING`, `FMINUS_LLVM_PACKAGE_VERSION`,
    `FMINUS_LLVM_MODE`, `FMINUS_LLVM_ASSERTIONS`, `FMINUS_BUILD_TYPE` (seen at FMinus `0a1d8c2`: check the pin,
    `3ed1ba6`). Per program: `Patch::Result::compileMs`, the frozen copy's `compileMs()`, `Patch::codeRanges`,
    the IR's sizes when kept, the loop budget, the bounds checks.
  - The plugin: `EditorPort::load()`, `Engine::Meters` (rate, voices, steals, dropped), the compiler's status,
    `shadercRuns()`, `shaderCacheScans()`, `shaderCacheFolder()`, `projectstore::parses()`,
    `EditorPort::desktop()` (the MCP link), `factorystore::State`, the stores' folders.
- The generated `Product.h` has the product's version and the FunkGui and JUCE versions; not the FMinus commit,
  not this repository's own commit.

## 2. Design

### 2.1 One card, with pages

`SettingsCard` becomes a card of pages: a rail of page names at the left, the page at the right, Done at the
bottom. About 780 x 560, scrolling inside a page when the window is short (it does today), Escape and a click
outside close it.

| Page | Holds | From |
|---|---|---|
| General | Theme (Graphite, Paper); Zoom; "What's new" (opens package 08's card); the version | here; 08 |
| Editor | Code text size; Format on Run | 06 part D; here |
| Audio and MIDI | Today's whole card, unchanged. Standalone only: in a host the page is not listed | here |
| Projects | The projects folder, with Show in Finder; Git: the git found, its version, a path to use instead | here; 05 |
| AI | The provider and model in words; "Provider and key..." opens the key dialog that exists (not a second copy of it) | here |
| Usage | Package 09's page | 09 |
| Network | "Get factory projects from the website" (the factory spec's switch); "Check for updates" and Check now; exactly what is sent | 08 |
| Debug | Section 2.3 | here |
| About | Name, version, the website, the licences' place | here |

- **A page is data.** `settings::Page { id, title, shown(), build(Places&), act(...), key() }`, registered in one
  table, so a later package adds a page as a file and a row, not a change inside the card. The devices' code
  moves into the Audio and MIDI page as it is.
- **The way in, in every host:** a gear button in the top bar, left of the theme button (`Panel.cpp`, the top
  bar's items; a tag of its own for the probes). The Standalone's Settings... and Cmd-, open the card at Audio
  and MIDI, as today; the muted-input notice too. `EditorPort` gains `settingsPage()` beside `settingsAsked()` so
  a caller can name the page. A headless Panel (a picture) has no gear to press but draws it.
- **Each control writes where its setting already lives**: the theme and zoom through UiPreferences as their
  buttons do (the top bar's theme button and the status bar's zoom pill stay); the others as their package says.
  The card keeps no copy of a value: it reads on each layout and follows `UiPreferences::revision()`.
- Selects are `dropdown::` triggers with the host's menu, as the devices' are.

### 2.2 What does not move

The chat's key dialog stays the place for keys and providers. The explorer's FOLDER rows stay the place for the
folder sync (it is the session's). The Projects card's view stays in its card.

### 2.3 The Debug page

Rows of `name  value` in sections, read-only, with **Copy all** (plain text, for a bug report) at the top. It
costs nothing unless open: the processor counts the pages open (`EditorPort::addDebugViewer`, as the IR view's
count), and only then gathers; the page reads once a second.

| Section | Rows |
|---|---|
| Versions | the product, its kind and version; this build's commit and build type; FMinus's version and pinned commit; LLVM's version, mode and assertions; FunkGui; JUCE; the MCP extension's version |
| Graphics (FunkGui) | the renderer and shader profile (`shaders::target()`); GPU or the CPU host; after F1 the GPU's name and limits; the surface attached or the fallback; frames a second, display-linked or the timer; frames drawn, skipped, unchanged; draw calls, vertices and bytes uploaded in the last frame; the transient buffer's size and overflows; instancing; image textures; GPU pictures supported; the editor's scale and zoom |
| Program (FMinus) | the program's id and files; compiled in N ms; its dials; modules; its machine code's ranges and bytes (`Patch::codeRanges`); loop budget and bounds checks on; the frozen copy (none, compiling, playing, its compile's ms); circuits (netlists, answers, tests in flight); the IR kept or not |
| Audio | the rate, the host's largest block, the load; the synth's voices, steals and dropped voices; the bypass; whether the frozen copy plays |
| Shaders | shaderc's path; runs this process; the cache's folder, entries and scans |
| Stores | the projects folder, its files, parses and reads this process; the chat's folder; the factory store's state and last check; the update check's (package 08); the preferences file |
| Claude Desktop | the MCP link: listening or why not, the socket folder, the instance's name, the target |
| Environment | which `<PREFIX>...` switches are set (names and values: none is a secret) |

Never a key, a chat's word, a file's text or the user's audio. Folders are shown as paths with Show in Finder
(through `plugin/Launch.h` on Linux, never a shell).

**Plumbing:**
- `EditorPort::debugRows()` returns the sections the processor knows (versions, program, audio, shaders, stores,
  Claude Desktop, environment), gathered on the message thread from getters that exist; the program's code ranges
  and IR sizes are taken from the compiler's status, never asked of a Patch that plays.
- The graphics rows are the host's: `gpu/CreateEditorGpu.cpp` and `gpu/CodeWindow.cpp` give the Panel a hook
  (`Panel::setHostDebug(std::function<std::vector<DebugRow>()>)`) that reads `EditorHost::diagnostics()` and the
  rest; `nogpu/SoftEditorHost` gives its own (the CPU host, frames, zoom). No bgfx header outside FunkGui's.
- `cmake/Product.h.in` gains `kFMinusCommit` (`FP_FMINUS_SHA`) and `kBuildType`: both change only with the pin or
  the preset. The checkout's own commit (`git describe --always --dirty` at configure, "unknown" outside a
  checkout) does not go there: almost every source includes `Product.h`, so a constant that changes with each
  commit would compile the whole tree again after each one, and would give CI's compiler cache a miss on every
  file. It goes in a generated file of its own (`generated/<key>/BuildCommit.h`, `kCommit`), which one `.cpp`
  includes and hands on through a function.
- After F1: `funkgui::gpuInfo()` fills the GPU's name and limits (package 10).

## 3. Work

1. The page table and the card's rail; the devices' code as the Audio and MIDI page; `gui.settings` green again.
2. The gear; the card opening in a host; General, Editor (Format on Run; the size when 06 part D is in), Projects,
   AI, About.
3. Debug: the rows, the viewer count, the host's hook, Copy all, the two new `Product.h` constants and the
   commit's own file.
4. Network, Usage and the Git rows arrive with their packages.

## 4. Tests

- `gui.settings` (it matches the section headings "AUDIO", "MIDI" by text: they are now inside a page): PAGES
  (the rail, the keys, the page kept while the card is open), HOST (a processor with no devices: the gear opens
  the card, no Audio and MIDI page), STANDALONE (Settings... lands on Audio and MIDI, every device row as
  before), GENERAL (theme and zoom write UiPreferences and a second Panel follows), EDITOR, DEBUG (the sections
  and rows named above are there; values that must be right: the version, the renderer's profile, the program's
  id; nothing is gathered while the page is closed; Copy all's text), A11Y, the least window. PNGs of each page
  in both themes.
- `gui.window` MENUS: Settings... still Cmd-,.
- `gui.dump`: the gear in the top bar's PNGs; MARK and the top bar's layout rows re-checked at the least width.

## 5. Docs

README: "Settings" (what each page holds; in a host, the gear), and "Theme, zoom, diagnostics" points at the
Debug page. CHANGELOG: Added. CLAUDE.md: the `SettingsCard` entry rewritten (pages, the table, the gear, a page
is data), the Standalone row (the card is no longer its alone), the Debug page's two rules (it gathers only
while open; it never shows a key, a chat's word, a file's text or audio).

## 6. Risks

- The top bar is crowded at the least width (the mark, the project pill, the presets, the window button, the
  Code / Panel switch, the load, the theme). The gear takes 28 px: `gui.dump`'s least-width rows decide whether
  the theme button moves into Settings' General page and the gear takes its place.
- If the FMinus version macros are not reachable through `fminus/FMinus.h` at the pin, the rows show the commit
  alone until the pin moves; do not include another FMinus header (`lint.layers`).
