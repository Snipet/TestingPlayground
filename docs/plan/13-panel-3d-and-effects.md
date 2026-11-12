# 13. `.gui`: 3D scenes and post-effects

Planning-document items: "GUI Panel: add support for 3D rendering (see FunkGui)", and the plugin's side of
FunkGui's "post-effect shaders". Size L, in two parts: scenes after FunkGui's F3, effects after F4 (package 10).
Decision D3: lit built-in shapes, OBJ models, meshes made from audio. Uses package 12's pointer properties.

## 1. What the user gets

- A `scene` in a panel: boxes, spheres, cylinders, cones, tori and planes, a model from an `.obj` file, a surface
  that is the sound's spectrum or waveform moving through time; a camera and a light; positions, turns and sizes
  that follow the program's params and fields. A 3D knob that turns with its dial; a cube that swells with the
  level; a spectrum laid out as terrain.
- An `effect` on a panel or a container in it: its picture drawn through a shader of the program's own (a bloom,
  a CRT's lines, grain, a vignette).
- Claude can write both, and sees them in view_panel and the GUI sandbox.

## 2. What exists

- No 3D anywhere (package 10, section 2). FunkGui's F3 gives `Canvas::scene`, a mesh registry with built-in
  shapes, `meshes::fromObj` and meshes that change each frame; F4 gives `Canvas::beginLayer`/`endLayer` and effect
  shaders that sample the layer.
- A `.gui` file's elements, limits and readers are `Source/fminus/GuiSpec.h/.cpp` and `PanelSpec.h`; their sizes
  `GuiLayout.cpp`; their items and drawing `Source/editor/GuiItems` and `GuiView`; CLAUDE.md's recipe "A .gui
  element" lists every place a new one touches.
- A property's value today is a number, a word or a param's or field's name. A field read by the panel is a tap
  (`Patch::tap`, 32 a panel: `kMaxDisplays`); a display's source may be a float, double, int or bool field or an
  array's element. `in` and `out` give the levels, the scopes' rings and the spectrum (`DisplayItem`, `Spectrum`:
  an FFT run only when the ring's samples change).
- A panel's files are `.gui`, `.sc`, `.sh` (`isPanelFile`, `Source/dsp/Sources.h`): a RUN that changes only those
  is applied with no compile (CLAUDE.md, "A panel's edit"). The files Claude may write or bring in are `.fm`,
  `.cir`, `.gui`, `.sc`, `.sh` (`isProgramFile`), 256 KB each; the folder sync mirrors text files up to 1 MB; a
  program has 16 files.
- A panel's shaders: four at most (`kMaxGuiShaders`), built by the bundled shaderc through the process's cache,
  with `#include "funk/text.sh"` and `"funk/shapes.sh"` from `Source/fminus/ShaderLib.cpp` (written for shaderc's
  HLSL front end on every renderer; no function there reads the clock).
- Without GPU pictures (Linux, or a build with `FP_WITH_GPU` off) a shader is a striped placeholder in pictures
  and the picture's text says so.

## 3. Part A: scenes (after F3)

### 3.1 The language

```
scene width=260 height=200 fov=35 distance=4 yaw=camYaw*360 pitch=20 background=none {
    light yaw=40 pitch=50 ambient=0.25 rim=0.2
    mesh cylinder size="1 0.4 1" rotate="0 cutoff*270-135 0" color=accent
    mesh "pointer.obj" position="0 0.3 0" rotate="0 cutoff*270-135 0" color=ink shade=flat
    surface spectrum of=out rows=48 cols=64 size="3 1 3" position="0 -1 0" color=accent shade=wire
}
```

- **`scene`** (a container whose children are only `light`, `mesh` and `surface`): the size properties every
  element has; the camera, an orbit about `target` (default the origin): `distance`, `yaw`, `pitch` in degrees,
  `fov`; `background` a colour or `none` (see-through, the default). It takes package 12's pointer properties,
  so an orbit by dragging is `x=camYaw y=camPitch drag=relative` with `yaw=camYaw*360 pitch=camPitch*180-90`:
  the camera is then two params, saved and automatable like any.
- **`light`** (one a scene; none: a default from the upper left): `yaw`, `pitch`, `color`, `ambient`, `rim`.
- **`mesh <shape>`** or **`mesh "<file>.obj"`**: `position`, `rotate` (degrees about x, y, z), `scale` or `size`
  (three numbers), `color` (a theme word or `#rrggbb`), `shade` (`smooth`, `flat`, `wire`, `unlit`), `gloss`,
  `spin` (turns a second about x, y, z). Shapes: `box`, `sphere`, `cylinder`, `cone`, `torus`, `plane`.
- **`surface spectrum|scope of=in|out`**: a grid of `rows` by `cols` (96 by 96 at most) whose height is the
  source: `spectrum`, the newest spectrum at the front row, each older one a row further back; `scope`, the
  waveform the same way. `floor=` and `ceiling=` in dB for a spectrum. The same placing and shading properties.
- **A value that moves.** Wherever a number goes in `position`, `rotate`, `scale`, `size`, `distance`, `yaw`,
  `pitch`, `fov`, `gloss`: a number, or `name`, `name*k`, `name*k+c`, `name*k-c`. `name` is a param (its
  normalised value, 0 to 1), a field (its own value, through a tap), or `in` or `out` (the level, 0 to 1). A
  value with spaces is quoted, as everywhere in `.gui`.
- **Limits**, each with a note when passed: 2 scenes a panel, 16 meshes and surfaces a scene, one surface a
  scene, an `.obj` of 256 KB and 20,000 vertices, a field binding is one of the panel's 32 taps.

### 3.2 `.obj` files (assumption A10)

- `.obj` becomes a panel file: `isPanelFile` and `isProgramFile` take it, so it is a tab (plain text: no
  language features, its `FileKind` its own), is saved with the session, projects and exports, mirrored by the
  folder sync, written and read by Claude's file tools, and a change to it alone is a panel's edit (no compile).
- A file dropped on the panel whose name ends `.obj` is added as a file (text files are; samples go to ASSETS).
- It is read with FunkGui's `meshes::fromObj` when the panel is read, off the message thread where the shaders
  are built; a file that does not read is a note on its line and the mesh is left out.
- The mesh is registered under an id made from the file's text (its hash), shared by every Panel that shows it
  and removed with the last, as the pictures are.

### 3.3 Drawing

- `PanelSpec.h`: `GuiKind::scene`, `light`, `mesh`, `surface`; a `Binding { float k, c; source }` for a value
  that moves; `kMaxScenes`, `kMaxSceneItems`.
- `GuiSpec.cpp`: the words, properties, bindings and notes; `GuiSpec.h`'s header. `GuiLayout.cpp`: a scene's
  size (260 x 200 unless given).
- `GuiItems`: `GuiSceneItem` builds a `funkgui::SceneDraw` each draw from its nodes and the values `GuiView`
  feeds it (params' values, the taps, the levels, `spin`'s time), and calls `Canvas::scene`.
  - Still: a scene whose values do not change is drawn once (FunkGui compares scenes). A param or field that
    moves is motion (`Stirred`, held a quarter second past the last move); `spin` and a surface with sound are
    motion while they last; hidden (a page not shown) it is none.
  - A surface's mesh is rebuilt on the message thread only when its source's samples changed (the spectrum's own
    rule), then `meshes::setDynamic`.
  - Accessibility: one image item, titled by the scene's `label=`, else "3D view"; the pointer properties'
    items from package 12 when it has them.
- `GuiView`: registers a panel's meshes with its shaders (`registerShaders`' place), a pool of 2 scenes.
- **Pictures:** `Picture.cpp` and the sandbox's helper draw scenes through the same items. Where a picture is
  SoftRaster's (Linux, `FP_WITH_GPU` off) a scene is FunkGui's placeholder and the picture's text says so with
  the shaders' ("Drawn without the GPU...").
- **The sandbox** has no audio: a surface shows the demo meters' steady tone; `values` may name a scene's params
  and fields as for any element.
- **The preview** (package 04) draws scenes as the PANEL view does.

### 3.4 Teaching and examples

- THE INTERFACE in `ChatPrompt.cpp`: the block above, the value forms, the limits, when to use a scene and when
  a shader (a shader for 2D and ray-marched pictures; a scene for solid things with a camera). The prompt's
  example must keep building (`plugin.chatexample`).
- One example a product with a scene: the synth's a 3D dial and a spectrum surface; the effect's a level cube.
  `FP_MCP_VERSION` raised.

## 4. Part B: effects (after F4)

### 4.1 The language

- On a container (`row`, `column`, `grid`, `section`, `box`, a `page`): `effect="glow.sc"` and
  `effectinputs="amount out"` (the words a shader's `inputs=` takes, 16 at most).
- For the whole panel, a line at the top level: `effect "crt.sc" inputs="amount"`.
- Two passes: `effect="blurh.sc blurv.sc"` (three files at most, in order; they share the inputs).
- 2 effects a panel (`kMaxGuiEffects`), counted apart from the 4 shaders. Never around the editor's own parts:
  an effect is inside the panel's area (assumption A9).

### 4.2 The shader

An effect is a fragment shader as a panel's shaders are, plus the picture it works on:

```
#include "funk/post.sh"
void main()
{
    vec4 c = layer(v_uv);                       // what the container drew
    c.rgb += bloom(v_uv, 0.6) * in_amount;      // funk/post.sh
    gl_FragColor = c * vignette(v_uv, 0.3);
}
```

- `Source/fminus/ShaderLib.cpp` gains `funk/post.sh`: `layer(uv)`, `layerSize()`, a 9-tap `blur(uv, dir)`,
  `bloom(uv, threshold)`, `vignette`, `scanlines`, `chroma` (colour fringes), `grain(uv, seed)` from a hash.
  By the library's rules: shaderc's HLSL front end on every renderer, and **no function reads the clock** (an
  effect that should move passes `u_time` itself, and then draws every frame, as any shader).
- `Source/fminus/Shaders.cpp`: an effect is built with FunkGui's effect prelude; the cache's key takes the kind.

### 4.3 Drawing

- `GuiView` wraps the container's drawing in `Canvas::beginLayer(rect)` and `endLayer(passes)`, the inputs fed as
  a shader's are.
- An effect draws again when its content or its inputs change, not otherwise: a still panel under a still effect
  costs nothing.
- Hit-testing and layout are untouched: an effect changes pixels, not where things are. An effect that moves the
  picture (a barrel distortion) therefore misleads the pointer; the prompt says to keep distortions small.
- Where FunkGui cannot run a layer (SoftRaster pictures, a build without the GPU) the content is drawn without
  its effect and the picture's text says so.

### 4.4 Teaching

THE INTERFACE gains the property, the top-level line, the shader's shape and `funk/post.sh`'s functions in a line
each. One example a product gains an effect.

## 5. Work

Part A: 3.2 (the file kind) can land before F3. Then the spec and layout, the item and `GuiView`, pictures and
the sandbox, the prompt and examples. Part B: the spec, `funk/post.sh` and the build, the wrap in `GuiView`,
pictures, the prompt.

## 6. Tests

- `fminus.gui` gains SCENE (every word and property; each value form; a param, a field, `in`; the limits' notes;
  a bad `.obj` named on its line) and EFFECT (the property, the line, three passes, the fourth refused, the
  count).
- `gui.panel` gains SCENE over a real processor: the `SceneDraw` an item makes (its camera, light, items and
  transforms) for a known panel; a dial turned turns its mesh; a field's value moves one; `in` and `out`; a still
  scene settles and a `spin` keeps the full rate and stops when its page is hidden (the frame counts, `Cadence.h`);
  a surface's mesh changes only when the ring's samples do; an orbit by package 12's properties writes its two
  params. And EFFECT: the layer's rect and passes; still content under a still effect draws nothing more.
- `gui.shaders` gains EFFECT (an effect builds; `funk/post.sh` compiles on the platform's renderer and as the
  other profile, as `text.metal` does for the text library; a change to `post.sh` rebuilds only what includes
  it) and SCENE's picture rows against the GPU (skipped where there are no GPU pictures: `noGpuPicturesSkip()`;
  the placeholder's words checked there instead).
- `plugin.sandbox`: `render` of a panel with a scene and an effect (GPU rows; the fallback's words).
- `plugin.import` and `plugin.folder`: an `.obj` comes in, goes out and syncs as a text file; `plugin.reload`:
  a change to an `.obj` alone is a panel's edit (no compile).
- `plugin.chatexample`, `plugin.mcp` (the guide's bounds), `fminus.examples`.
- PNGs: `gui.panel.scene*.png`, `gui.panel.effect*.png`, both themes; look at them on a Mac (the GPU's picture).

## 7. Docs

README, "The interface": scenes, `.obj` files, effects, with pictures. CHANGELOG: Added. CLAUDE.md: the `.gui`
recipe's new kinds; `.obj` among the panel's files (three kinds of file become four in "The code editor");
`funk/post.sh` in the shaders' library's recipe; the scene's motion rule; FunkGui's pin line.

## 8. Risks and what was left out

- **Linux pictures.** view_panel and the sandbox show a scene as a placeholder and an effect not at all there
  (no read-back from Vulkan in FunkGui), so Claude cannot see its 3D work on Linux. On screen both are real. If
  that matters, the answer is in FunkGui (a Vulkan read-back), not here.
- **Cost.** A scene is a render target, a surface a mesh rebuilt as audio comes. The limits (2 scenes, one
  surface each, 96 x 96) bound it; `gui.panel`'s RATE rows are the check that a still scene is free.
- **OBJ as text** caps models at 256 KB. A binary asset kind for models, textures on meshes, glTF and PBR are
  later (D3).
- **16 files.** A program with several `.obj` files, shaders and includes meets the limit sooner (D9 keeps it).
