# 10. FunkGui: input, the renderer split and a GLFW host, 3D scenes, effect layers, examples

Planning-document items: "FunkGui: add support for rendering 3D geometry", "add support for post-effect shaders",
"add examples, including mostly ones not relying on JUCE"; and what packages 01, 07, 11 and 13 need from FunkGui.
Size L: five releases. Decisions D3 and D8. The work is in the FunkGui repository, under its own rules.

## 1. FunkGui's rules, for whoever takes a release

From FunkGui's CLAUDE.md (read it first; it wins):

- Work is a card with an `OWNS` list, in a worktree and branch the lead makes; one handoff commit; **the lead
  merges, tags, bumps the version and writes CHANGELOG.md**. Never bless a golden, never edit `SEED.tsv`.
- **Public headers are API** (`include/funkgui/**`): every signature below is a proposal for the lead to approve,
  and each release is a MINOR bump. Target names are frozen: new ones are added, none renamed.
- bgfx and Objective-C only under `gpu/`; everything else builds with `FUNKGUI_WITH_BGFX=OFF` and with
  `FUNKGUI_WITH_JUCE=OFF`. Tests are `fg.*`, self-registering, `-Werror`.
- Done: the `agent`, `agent-gui` and `nojuce` presets (and `web` when a JUCE-free build compiles the change) build
  with no warning and `tools/verify.sh` exits 0; **existing frames' goldens do not move**.
- SoftRaster mirrors the UI's fragment shaders line for line; a GPU feature it cannot mirror has a stated
  stand-in (custom shaders: a striped placeholder).
- The `Prim` struct, `vs_ui.sc` and `vs_ui_inst.sc` stay as they are: new kinds of draw are kept beside the
  primitives, as `ShaderDraw` and `ImageDraw` are (`canvas/PrimList.h`).

## 2. What exists (v0.26.1)

- **Input** (`include/funkgui/panel/Input.h`): `PointerEvent{x, y, mods, clicks, popup}`: no button, so a middle
  press arrives as an ordinary one; `WheelEvent{x, y, dx, dy, smooth, reversed, inertial, mods}`; no pinch.
  `HeadlessHost::wheel(x, y, dy, smooth, mods)` has no `dx`, `reversed` or `inertial`, and its Ctrl means popup.
- **Drawing:** a `Canvas` records a `PrimList` (four primitive kinds, plus `ShaderDraw`s and `ImageDraw`s at
  their places in the order). `BgfxSink::submit` draws runs of primitives and each shader or image draw in
  recording order, in one bgfx view an editor (ids 0 to 16), with alpha blending and no depth; each vertex shader
  makes its own NDC from `u_viewSize`. The only offscreen target is the picture read-back (`GpuPicture.cpp`,
  views `maxViews-2` and `-1`), which Vulkan cannot do.
- **Custom shaders** (`gpu/ShaderLibrary.h`): a fragment shader over a quad, uniforms `u_viewSize`, `u_rect`,
  `u_params[4]`, up to four textures given by the host; any other uniform refused.
- **Hosts:** `EditorHost` (JUCE), `HeadlessHost` (synthetic input, SoftRaster), `WebHost` (Emscripten, WebGL2).
  **`FunkGui::gpu` links JUCE**: `FramePump` is a `juce::Timer`, the Linux surface takes JUCE's X11 display, the
  picture read-back pumps JUCE's message loop, menus and choosers are JUCE's. With JUCE off, bgfx is off too.
  `BgfxContext`, `BgfxSink` and `ShaderLibrary` themselves take a `void*` window handle and no JUCE type.
- **Text without JUCE:** the atlas is loaded from `fonts/FunkGuiAtlas-macos.bin` (`src/nojuce/`).
- **Examples:** none. `tools/GalleryApp` (JUCE), `tools/GalleryWeb` (Emscripten), `tools/GalleryProbe`
  (headless), and a 25-line sketch in the README.
- No version function, no renderer or GPU name in the API.

## 3. The releases

Suggested order: F1 first (three packages here wait on it). F3 and F2 can run beside each other, then F4, then
F5. Version numbers are the lead's; "0.27" and so on below are placeholders.

### F1 (0.27): input, scrolling constants, debug information. Size M

For packages 01, 07 and 11.

- **`Input.h`:**
  ```cpp
  enum class Button : uint8_t { left, middle, right };
  struct PointerEvent { ...; Button button = Button::left; };       // popup stays: right-click or ctrl-click
  struct MagnifyEvent { float x = 0, y = 0; float scale = 1.0f;     // the factor since the last event: > 1 opens
                        Mods mods{}; };
  ```
  `Panel::magnify(const MagnifyEvent&)`, virtual, false by default (the host then does nothing more).
- **`EditorHost`:** sets `button` from JUCE's modifiers on down, drag and up; overrides `mouseMagnify` and sends
  `magnify`. A middle press was delivered as a left one until now: a Panel that does not read `button` behaves as
  before. `WebHost`: `button` from the DOM event; a browser's pinch (ctrl-wheel) stays a wheel.
- **`HeadlessHost`:** `pointerDown/Drag/Up` with a `Button`; `wheel(const WheelEvent&)` whole (dx, reversed,
  inertial, mods with Ctrl that is not popup); `magnify(...)`. The old signatures stay.
- **`funkgui/widgets/Scroll.h`:** the TextEditor's wheel rule and bar as public constants and two functions
  (`wheelPx(delta, smooth, rowHeight)`, `thumb(area, content, view, offset)`), which `TextEditor.cpp` then uses
  itself. A host's own lists scroll as the editor does by construction (package 01 drops its copies).
- **Debug:** `funkgui::version()` (core); `funkgui::gpuInfo()` (gpu): the renderer's name, the vendor and device
  ids, the largest texture, the transient buffer's size, instancing, image textures and shaders held, pictures
  supported. Read-only, message thread.
- Tests: `fg.host.buttons`, `fg.host.magnify` (HeadlessHost round trips), `fg.widgets.scroll` (the TextEditor
  moves what `wheelPx` says), `fg.gpu.info`.
- Here afterwards: `nogpu/SoftEditorHost` carries the same input changes by hand; the pin moves.

### F2 (0.28): the renderer without JUCE, and a GLFW host. Size L

- **Split, behind the same names.** A new target `FunkGui::render`: `BgfxContext`, `BgfxSink`, `ShaderLibrary`,
  `ImageGpu`, the picture read-back and the frame pump's core, with three small seams where JUCE is today:
  - `FrameTicker`: who calls a frame (JUCE's timer or display link; GLFW's loop);
  - the window surface: each platform's `createRenderView` takes raw handles (on Linux the X11 display and
    window from its host, not from JUCE);
  - `pumpMainLoop()`: what the picture read-back calls while it waits.
  `FunkGui::gpu` becomes `render` plus the JUCE pieces (`EditorHost`, `HostServicesJuce`, the JUCE ticker):
  consumers link what they linked and see no change. The proof: FCompressor and this plugin build and pass
  unchanged against it.
- **`FunkGui::glfw`** (`include/funkgui/glfw/GlfwHost.h`, `src/glfw/`), built only with `FUNKGUI_WITH_GLFW=ON`
  (off by default, so no consumer fetches GLFW): a window with a `Panel` in it.
  - Native handles from GLFW (Cocoa, Win32, X11; XWayland on Wayland, as the JUCE host), high-DPI scale, resize.
  - Input into the Panel: pointer with its button, the wheel (GLFW gives no "precise" flag: `smooth` is true on
    macOS, false elsewhere, and the README says so), keys and typed characters, file drops, the cursor shapes.
  - `HostServices`: the clock, the clipboard, full-rate nudges, the unbounded drag (GLFW's disabled cursor);
    menus and file choosers return "not shown" in this release (an example that needs one says so).
  - The frame loop: `Panel::needsDraw`, 60 a second while something moves and 12 when calm, as `FramePump`.
  - GLFW 3.4, pinned, from the machine's dependency cache when it is there, as bgfx.cmake is.
- **A first three examples** (section F5's rules): `hello`, `widgets`, `shader`.
- Tests: the split is covered by every existing `fg.gpu.*`; `fg.glfw.smoke` (label `live`: it needs a display)
  opens a window, draws a frame, sends a click and closes; the examples' own (F5).
- Risk: the Linux surface and the read-back are the two places JUCE is woven in deepest. Do the seams first, with
  no GLFW, and land that alone if the release must be cut.

### F3 (0.29): 3D scenes. Size L

Decision D3: lit built-in shapes, OBJ models, meshes that change every frame (surfaces made from audio).

- **API, all in core** (no bgfx type; proposed):
  ```cpp
  // funkgui/core/Math3d.h: Vec3, Mat4, translate/rotate/scale, and nothing of projection (the GPU layer owns it)
  // funkgui/canvas/Meshes.h: a process-wide registry, as images:: is
  namespace meshes
  {
      struct Vertex { float px, py, pz, nx, ny, nz; };                       // 24 bytes
      bool set(uint32_t id, const Vertex* v, uint32_t nv, const uint32_t* i, uint32_t ni, std::string& why);
      bool setDynamic(uint32_t id, const Vertex* v, uint32_t nv, const uint32_t* i, uint32_t ni, std::string& why);
      void remove(uint32_t id);  bool has(uint32_t id);
      enum class Shape : uint8_t { box, sphere, cylinder, cone, torus, plane };
      uint32_t builtin(Shape s, int detail = 0);                             // made on first use, kept
      bool fromObj(std::string_view text, std::vector<Vertex>& v, std::vector<uint32_t>& i, std::string& why);
  }
  // funkgui/canvas/Scene.h
  struct Camera     { Vec3 eye, target, up{0,1,0}; float fovY = 35.0f, nearZ = 0.1f, farZ = 100.0f; };
  struct SceneLight { Vec3 direction; Rgba8 colour; float ambient = 0.25f, rim = 0.0f; };
  struct SceneItem  { uint32_t mesh; Mat4 model; Rgba8 colour;
                      enum class Shade : uint8_t { smooth, flat, unlit, wire } shade; float gloss = 0.3f; };
  struct SceneDraw  { Rect rect; Camera camera; SceneLight light; Rgba8 background{};   // alpha 0: see-through
                      const SceneItem* items; int count; };                              // 64 at most
  void Canvas::scene(const SceneDraw&);      // recorded beside the primitives, at its place in the order
  ```
  Limits, asserted and said in the header: 64 items a scene, 8 scenes a frame, 200,000 triangles a mesh.
  `fromObj` reads positions, normals and faces (triangles and polygons fanned), makes flat normals where a file
  has none, and ignores materials and textures.
- **GPU:** each scene is drawn into an offscreen target of its rect's size, colour and depth, multisampled where
  the renderer can, in a view of its own that runs before the editor's (`setViewOrder`); then its colour is
  drawn into the UI as an image is, at the scene's place in the recording order, inside its clip. So 2D draws
  over and under a scene as it does an image, and `Prim` and the UI's shaders are untouched.
  - Targets are pooled by owner, slot and size, and let go a second after their last use.
  - One mesh program, `vs_mesh.sc` and `fs_mesh.sc`: a key light, a sky-and-ground ambient, an optional rim, a
    gloss highlight; flat shading from screen-space derivatives; wire by barycentrics. Depth test and write, back
    faces culled. Compiled for Metal, SPIR-V and Direct3D 11 as the UI's are.
  - The projection is built in the GPU layer from the `Camera` (the renderers differ in depth range and
    handedness; bgfx's caps say which).
  - Static meshes live in vertex and index buffers made at `set`; dynamic ones are updated in place.
- **Everything else that sees a frame says what it does with a scene:** SoftRaster draws a placeholder (the
  rect, tinted, with a wire cube: `scenePlaceholder`, as `shaderPlaceholder`); the web sink draws nothing in this
  release; the picture read-back draws scenes for real; the frame comparison, the fingerprint and the dump take
  a scene's camera, light, items and each dynamic mesh's change count, so an unchanged scene costs no frame.
- Tests: `fg.canvas.scene` (recording, comparison, dump), `fg.meshes.obj` (a cube, a file with no normals, bad
  files refused in words), `fg.gpu.scene` on Metal, pixel checks: a near box hides a far one whatever the draw
  order; a 2D rect recorded after the scene is on top, one before is under; the clip cuts it; a see-through
  background shows what is behind; an unchanged scene is not drawn again; `fg.soft.scene` (the placeholder).
  No existing golden moves.

### F4 (0.30): layers and effect shaders. Size L

- **API** (proposed): `Canvas::beginLayer(const Rect& r)` and `Canvas::endLayer(const EffectPass* passes, int n)`
  with `struct EffectPass { uint32_t shader; float params[16]; int nParams; }`. What is recorded between them
  (primitives, text, images, shader draws, scenes) is drawn into a texture of `r`'s size, then drawn at `r`
  through each pass in turn (three at most, ping-ponged), the last onto the frame, at the layer's place in the
  order. Two layers deep at most, four a frame; past that the content is drawn with no effect and a counter says
  so.
- **An effect shader** is a custom fragment shader that samples the layer: `SAMPLER2D(s_layer, 0)`, bound by
  FunkGui; `shaders::set` learns that one reserved name and still refuses any other uniform. It gets the uniforms
  a custom shader gets (`u_rect`, `u_viewSize` with the time, `u_params`). `shaders::effectPrelude()` beside
  `fragmentPrelude()`.
- **Stand-ins:** SoftRaster, the web sink: the layer's content drawn as it is, no effect (the content stays
  visible, unlike a placeholder). The picture read-back: for real. The frame comparison takes layers.
- Tests: `fg.canvas.layer`; `fg.gpu.layer` on Metal: an identity effect gives the pixels of no layer at all; an
  invert; two passes; a layer in a layer; a scene inside a layer; the clip; a layer whose content did not change
  is not drawn again.
- Risk: text inside a layer is rendered at the layer's resolution, so the layer's target is made at the frame's
  physical scale, never the logical one.

### F5 (0.30.x): the examples. Size M

- **`examples/<name>/`**, each a `CMakeLists.txt`, one `main.cpp` kept short, and a paragraph in
  `examples/README.md` with its picture. Built by an `examples` preset (GLFW on); CI builds them on the three
  systems.
- Every windowed example takes `--frames N --png <file>`: it draws N frames with no window (HeadlessHost) and
  writes a picture, which is its test (`fg.examples.<name>`, label `examples`) and its README's image.

| Example | Shows | JUCE |
|---|---|---|
| `hello` | A Panel, a label, a dial, a window | no |
| `widgets` | Every widget, both themes, zoom | no |
| `text-editor` | The TextEditor with a highlighter | no |
| `shader` | A custom fragment shader with inputs and a texture | no |
| `images` | Pictures, mips, rounded corners | no |
| `meters` | A scope and a spectrum fed by a made-up signal; unchanged frames left undrawn | no |
| `scene` | Lit built-in shapes, an orbiting camera (after F3) | no |
| `obj-viewer` | A model loaded from a file given on the command line (after F3) | no |
| `surface` | A mesh that changes every frame: a waterfall of a made-up spectrum (after F3) | no |
| `effects` | A layer with a two-pass bloom and a CRT pass (after F4) | no |
| `headless-png` | No window at all: a frame to a PNG, for build servers | no |
| `web` | The same Panel in a browser (the existing GalleryWeb, tidied into `examples/`) | no |
| `juce-plugin` | A minimal JUCE plugin with a FunkGui editor (EditorHost), the path the products use | yes |

- FunkGui's CMake says later cards add files and no CMake edits: the lead adds the `examples/*/CMakeLists.txt`
  glob and the preset once, in F2.

## 4. What this plugin does after each release

| After | Here |
|---|---|
| F1 | Pin move; `SoftEditorHost` gains buttons and magnify; packages 01 (the shared constants), 07 (GPU rows), 11 |
| F2 | Pin move only (nothing here uses GLFW); `dev-verify` proves the split changed nothing |
| F3 | Pin move; package 13's scenes; `Picture.cpp` and the sandbox's helper draw them |
| F4 | Pin move; package 13's effects |

Each pin move is `cmake/Deps.cmake`'s three values together, CLAUDE.md's FunkGui line, and CHANGELOG.

## 5. Risks

- **Vulkan has no picture read-back** in FunkGui, so on Linux view_panel's pictures show a scene as its
  placeholder and a layer's content without its effect; on screen both are real. The plugin's words for that
  exist ("Drawn without the GPU...").
- **Hosts with many editors:** each scene and each layer is a render target. The pools are per editor and
  bounded (8 scenes, 4 layers); `fg.gpu.*` must show two editors with scenes drawing without stealing views
  (ids 0 to 16 are the editors' today).
- **Direct3D 11's shaders are compiled only on Windows**, so `vs_mesh`/`fs_mesh` and the effect prelude are not
  proven there until a Windows build runs: FunkGui's v0.26.0 to v0.26.1 was exactly this kind of fault.
- **The web renderer** gains neither scenes nor layers here. An example that needs them is not built for the web.
