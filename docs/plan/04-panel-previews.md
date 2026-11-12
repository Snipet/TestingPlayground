# 04. Panel previews in the Projects card

Planning-document item: "Add panel previews to Projects in the Project/Preset explorer. When the user clicks on a
Project without triggering it to open, the preview image of its panel should show." Size M to L. Needs 03 (the
reader of a version's content).

## 1. What the user gets

Choosing a project in the Projects card (a single click, which does not open it) shows a picture of its panel
beside the versions. Choosing a version shows that version's panel. It works for every project there is,
factory projects too, with no need to have opened or saved it first.

## 2. What exists

- **Pictures of the panel** are view_panel's: `picturePanel` (`Source/editor/Picture.cpp`) builds a whole headless
  `ui::Panel` over an `EditorPort` and draws it with `picture::draw` (`PictureDraw.cpp`: FunkGui's GPU picture,
  else SoftRaster with each shader a striped placeholder). Message thread only. The first GPU picture starts bgfx
  headless (40 to 190 ms), the next about 25 ms; the context is held 10 s (`kPictureHoldSeconds`).
- It needs **the program that plays**: `port.status()` for the controls and the panel. `EditorPort` has 113 pure
  virtuals and one implementation, the Processor, so a stand-in port for a project that is not open is not a small
  thing.
- **The GUI sandbox draws a panel without compiling anything** (`Source/sandbox/`): `readProgram(files,
  instrument)` (`Dials.h`) reads the dials from `Lang`'s symbols, circuits' dials included; `readGuiSpec` reads
  the `.gui`; `View` is a small `funkgui::Panel` holding the editor's own `GuiView` over mock ports and demo
  meters; `picture::draw` draws it. `plugin.sandbox` MATCH holds that picture to view_panel's, to the pixel but
  the displays. Those three sources are compiled into the helper only; what they stand on (`GuiView`, `GuiItems`,
  `GuiLayout`, `Looks`, `DisplayItem`, `PictureDraw`, `GuiSpec`, `PanelSpec`, `Lang`, `Shaders`) is
  `FP_SANDBOX_SHARED`, in the plugin too. Moving them is allowed (`lint.layers` forbids only that `Source/sandbox`
  include plugin, audio or compiler code).
- **A program with no `.gui` file** (plain rows, or `#pragma panel` lines) is laid out by `layoutDials`
  (`Source/editor/DialLayout.h`, which includes only `Looks.h`) and drawn inside `Panel.cpp`. The sandbox does not
  draw it: it substitutes a blank `.gui` of the dials.
- **Images on the canvas**: `funkgui::images::set(id, w, h, rgba)` (4,096 a side at most, mipmapped), drawn with
  `Canvas::image`; `Panel::syncPictures` counts each id's use across Panels and removes it with the last.
- The card's right side today: the versions list (56 % of the height) over the presets list.

## 3. Design (assumption A4: drawn on demand, nothing stored)

### 3.1 A compile-free picture of a version's panel

`Source/editor/Preview.h/.cpp`:

```cpp
namespace preview
{
    struct Request { Sources files; std::vector<ProjectDial> dials; bool instrument; int theme; };
    struct Picture { funkgui::Image image; bool gpu = false; std::string why; };   // straight RGBA8
    Picture draw(const Request& r, int maxWidth);     // message thread only (bgfx)
}
```

- The dials come from `readProgram` (moved from `Source/sandbox/Dials.*` to `Source/fminus/ProgramDials.*`: it is
  `Lang` only, so it stays FMinus-free and the sandbox keeps using it), set to the version's saved values by name.
- **With a `.gui` file:** the sandbox's `View` (moved to `Source/editor/PanelView.*`, shared with the helper) over
  `readGuiSpec`, at the PANEL view's area for the default editor size (`ui::panelViewArea`: 853 x 612), drawn by
  `picture::draw`. This is the code the PANEL view draws with, so the picture is the panel's but for the displays,
  which show the sandbox's demo values.
- **Without one:** `readPanelSpec`, then `layoutDials`, then a static draw of each cell with `looks::` (the dial
  styles, the displays' frames, the section titles), in a new `preview::drawRows`. A probe holds it to
  view_panel's picture of the same program (section 5), so it cannot drift from `Panel.cpp`'s drawing.
- Shaders are built through the process's `shaderCache()` before the draw, off the message thread (a hit costs
  nothing; a miss about 60 ms a shader; no shaderc: the placeholder).
- The image is scaled to `maxWidth` (640 px) before it is registered: about 1.2 MB a picture.

### 3.2 Asking, drawing, keeping

- `EditorPort`:
  ```cpp
  struct PreviewState { enum class Status { none, reading, drawing, ready, failed } status; uint32_t image = 0;
                        int w = 0, h = 0; std::string why; };
  virtual PreviewState previewPolled(const std::string& project, int version, int theme) = 0;
  ```
- The Processor: the version's content from 03's reader (`versionPolled`'s table); the shaders built on the
  Reader's thread; the draw in `poll()`, **one preview a poll at most**, and none while a picture for Claude
  waits (view_panel comes first).
- A table of the newest 12 pictures, keyed by the project file's name, time and size, the version, the theme and
  the product; each registered with `funkgui::images` under an id of its own, counted as `syncPictures` counts
  the chat's, released ten seconds after the Projects card closes (the pictures' hold).
- The card asks 150 ms after the choice rests, so the arrow keys through a list draw nothing on the way.

### 3.3 The card

- The right side becomes: the preview pane at the top (the panel area's shape, about 1.39 : 1; 330 x 237 at the
  card's 860 px), then the versions and the presets as now, each a little shorter. `kBoxH` grows from 640 to 720
  where the window allows; in a window too short for both the pane and four rows of each list, the pane goes (the
  least window keeps the lists).
- In the pane: the picture, fitted, on the program card's colour; "Reading..." or "Drawing..." while it comes; "No
  preview: <why>" when it fails (a program whose text does not read). Under it one line: the project's name and
  "v12" when a version is chosen.
- A click on the pane does nothing (opening stays the double-click's and the Open button's).
- Accessibility: one image item, titled "Preview of <name>'s panel".

## 4. Work

1. Move `Dials` and `View` out of `Source/sandbox` into shared code; the helper builds and `plugin.sandbox`
   passes unchanged.
2. `preview::draw` for a `.gui` program; then `drawRows` for the rest.
3. `previewPolled`, the Processor's table and its place in `poll()`.
4. The card's pane and layout; the Panel's picture counting.
5. Probes and docs.

Cut for a first pull request: steps 1 to 4 for `.gui` programs only, the others showing "No preview (this program
has no panel.gui)"; `drawRows` second.

## 5. Tests

- New probe `gui.preview`: MATCH (the prompt's example: `preview::draw`'s picture against view_panel's panel
  picture of the same files at the same size, the same pixels outside the displays, as `plugin.sandbox` MATCH);
  ROWS (a plain program and a `#pragma panel` one: `drawRows` against view_panel's, the same cells in the same
  places, the dials' pixels within a small bound); VALUES (the version's saved dial values show); SHADER (built,
  or the placeholder with why); FAILS (a text that does not read). GPU rows skipped on Linux through
  `noGpuPicturesSkip()`, as view_panel's are.
- `gui.projects` gains PREVIEW: the pane fills some ticks after a choice; a version's click changes it; nothing
  is drawn while the card is closed or while the arrow keys run through the list; one draw a poll at most; twelve
  kept; the least window has no pane; a factory project's. PNG `gui.projects.preview*.png`, both themes.
- `plugin.projects`: no parse on the message thread for a preview (`projectstore::parses`).

## 6. Docs

README, "The editor": the Projects card's preview, and that its displays show sample values. CHANGELOG: Added.
CLAUDE.md: `Preview`, `PanelView` and `ProgramDials` in "What goes where" (the sandbox's row loses `Dials` and
`View`); the rule that a preview is drawn in `poll()`, one a poll, after Claude's pictures.

## 7. Risks and later

- A preview is not stored, so a project folder copied to another machine draws its previews again there; that
  costs milliseconds. Storing an exact capture in the file (an optional field written at save) is the later step
  if the website wants pictures of the factory projects; nothing here blocks it.
- On Linux a GPU picture is SoftRaster's, so a panel's shaders are placeholders in previews, as in view_panel.
- The theme is the user's at the time; a theme switch draws the shown one again (the key holds the theme).
