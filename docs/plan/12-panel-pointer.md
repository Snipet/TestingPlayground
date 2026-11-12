# 12. `.gui`: the pointer drives params and shaders

Planning-document item: "Add support for handling basic mouse events on GUI components. This will enable more
complex UX experiences, and the AI assistant should be able to take advantage of this." Size M to L. Decision D2:
the pointer writes params, and the pointer goes into shaders. Needs nothing from FunkGui.

## 1. What the user gets

A panel can have controls of its own making. An element says which params the pointer writes: where it is
pressed, whether it is held, the wheel. A shader element can also read the pointer, so a control can be drawn
entirely by a shader and dragged like any knob. The program reads plain params, so everything it does with them
is saved with presets, automated by the host and learnable by MIDI, and Claude can write such a control in one
`.gui` file and one `.sc` file.

## 2. What exists

- The `.gui` language (`Source/fminus/GuiSpec.h`'s header; kinds in `PanelSpec.h`, `GuiKind`): containers (`row`,
  `column`, `grid`, `section`, `box`, `tabs`/`page`), `label`, the controls (`knob`, `slider`, `switch`,
  `buttons`, `menu`, `xy`, `envelope`, `keyboard`), the displays, `shader`. A control binds to a param by name.
- **A panel reaches the program only through dials** (a `funkgui::ParamPort` each, written through the
  `GestureController`: `beginDrag`, `dragTo`, `endDrag`, `tap`, `tapMany`, `wheelTo`) and the keyboard's notes
  and wheels. `xy X Y` is the nearest thing: a pad that writes two float params on press and drag.
- A program has 64 dials: its first 32 params are the host's parameters, the next 32 the panel's alone
  (`Source/dsp/Controls.h`, `isPanelDial`): no host parameter, never automated, kept in the session.
- **The shader element takes no pointer**: `GuiShaderItem::pointerDown` is empty and it is not focusable. Its
  inputs are 16 floats (`inputs="a b out"`: params, fields, `in`, `out`), read in the shader as `in0`... and
  `in_<name>`; it draws again only when an input changes or its code reads the time.
- Items get `pointerMove`, `pointerExit`, `pointerDown`, `pointerDrag`, `pointerUp`, `doubleClick`, `wheel`,
  `key`, and a `hovered` flag each tick (`Source/editor/Items.h`); the Panel sends a press to the first shown
  item that contains it, and drags to the item pressed.
- Every control has a dial menu on right-click (MIDI learn, the host's menu: `GuiView::Hooks::dialMenu`), is
  focusable, and is exposed to a screen reader.
- The GUI sandbox draws the same items over mock ports (`Source/sandbox/`), with no plugin behind them;
  view_panel and the sandbox's `render` take `values` that name dials, fields, `in` and `out`
  (`mcp::kPanelValues`).
- The language is taught in the prompt's THE INTERFACE (`Source/plugin/ChatPrompt.cpp`, `kPanel`) and in the MCP
  guide's interface part.

## 3. The language

### 3.1 Pointer properties

Five properties, on `shader`, on a new element `area`, and on the elements that take no press of their own
(`box`, `row`, `column`, `grid`, `section`, `label` and every display):

| Property | Param it names | What is written |
|---|---|---|
| `x=` | a float | The pointer's place across the element, left 0 to right 1, as a knob's turn maps its range and taper |
| `y=` | a float | Its place up the element, bottom 0 to top 1 |
| `down=` | a bool, an int or a float | Its highest value while the element is pressed, its lowest when let go |
| `wheel=` | a float or an int | Nudged by the wheel over the element, as a knob's wheel |
| `hover=` | a bool, an int or a float, **panel-only** (assumption A11) | Its highest value while the pointer is over the element |

and three that shape them:

| Property | Values | Meaning |
|---|---|---|
| `drag=` | `absolute` (default), `relative` | absolute: `x` and `y` jump to the press and follow it. relative: they move by the pointer's travel, 200 px for the whole range, Shift a quarter as fast, as a knob |
| `release=` | `keep` (default), `spring` | spring: `x` and `y` go back to their defaults when let go |
| `cursor=` | `hand`, `cross`, `grab`, `leftright`, `updown` | the pointer's shape over it |

- One press is **one gesture**: `x`, `y` and `down` begin together at the press and end together at the release
  (`beginDrag` on each port, `dragTo` as it moves, `endDrag`), so the host records one automation gesture, the
  journal one change, and undo takes it whole.
- A double-click puts `x` and `y` back to their defaults (as `xy`). A right-click opens the dial menu of the
  first param named (MIDI learn works on it as on any dial).
- A container with pointer properties gets the presses its children do not take: a knob inside still turns.
- A param may be named by one pointer property in a panel and also by ordinary controls.
- Notes, as `GuiSpec` gives them, and the property is then ignored: an unknown param; a type that does not suit;
  `hover=` naming a host parameter ("hover= needs a panel-only param: one after the program's 32nd"); a pointer
  property on a control that takes presses itself (`knob`, `slider`, `xy`, ...); more than 16 elements with
  pointer properties (`kMaxPointers`).

### 3.2 `area`

`area x=padX y=padY down=gate width=200 height=120`: an element that draws nothing and has only pointer
properties and a size (`width`, `height`, `grow`, and the rest every element takes). It is for a region of a
panel whose picture something else draws: a display behind it in a `box`, or nothing at all.

### 3.3 The pointer in a shader

Four more words for `inputs=`: `pointer.x`, `pointer.y` (0 to 1 inside the shader's own rect, the last place
while the pointer is elsewhere), `pointer.down` (1 while this element is pressed), `pointer.hover` (1 while the
pointer is over it). In the code they are `in_pointer_x`, `in_pointer_y`, `in_pointer_down`, `in_pointer_hover`.
They count among the 16 inputs.

They need no param: a shader can light up under the pointer with nothing written anywhere. Together with the
pointer properties they make a control:

```
shader "dial.sc" inputs="cutoff pointer.hover pointer.down" y=cutoff drag=relative cursor=updown width=72 height=72
```

A shader that reads the pointer draws again when the pointer's inputs change, not every frame.

## 4. Work

1. **`PanelSpec.h`:** `GuiKind::area`; on `GuiNode` a `PanelPointer { int x, y, down, wheel, hover; Drag drag;
   Release release; Cursor cursor; }` of slots (-1: none); `kMaxPointers = 16`; the four `pointer.*` shader
   input sources beside `param:N`, `field:D`, `in`, `out`.
2. **`GuiSpec.cpp`:** the properties, `area`, the notes; `GuiSpec.h`'s header gains the section.
3. **`GuiLayout.cpp`:** `area`'s size (its `width` and `height`, else 120 x 80).
4. **`GuiItems`:** one `PointerSink` that holds the ports and does the gesture (press, drag, release, wheel,
   double-click, the keys below), used by a new `GuiAreaItem`, by `GuiShaderItem`, and by a light item made for a
   container, label or display that has pointer properties (placed behind its children in the Panel's order, so
   they are hit first). Its motion is the dials' own; its `drawKey` holds the pointer inputs.
5. **`GuiView`:** makes the items; feeds `pointer.*` into each shader's inputs in `tick` (with the fields and
   params it feeds now); the dial menu hook; a pool beside the others (`kPool`).
6. **Keyboard and screen reader:** an element with `x`, `y` or `down` is focusable. Arrows move `x` and `y` by
   1 % (Shift 10 %); Space or Return holds `down` while held; Home puts both back. Accessibility: a slider for
   each of `x` and `y`, a button for `down`, titled by the params' names.
7. **`hover=`:** written through the panel dial's port with no gesture (a panel dial has none), on enter and
   exit; never while a card is open over the panel.
8. **The sandbox and pictures:** the mock ports take the writes as they are; `values` (view_panel's and the
   sandbox's, one shared wording, `mcp::kPanelValues`) gains `pointer.x`, `pointer.y`, `pointer.down`,
   `pointer.hover`, applied to every shader in the picture, so Claude can look at a control hovered or pressed.
9. **Teaching:** THE INTERFACE in `ChatPrompt.cpp` gains a short paragraph, the two tables in a line each and the
   shader-dial example (it must build: `plugin.chatexample` and `gui.shaders` PROMPT compile the prompt's
   examples); the MCP guide's interface part follows from it; `FP_MCP_VERSION` raised. The `values` words cost a
   few bytes of the tool budget: `plugin.mcp` prints the slack.
10. One example for each product in `Source/fminus/examples/<key>/` whose panel has a shader-drawn control (its
    `fminus.examples` rows as any example's).

## 5. Tests

- `fminus.gui` gains POINTER: each property parses to its slot; `area`; the notes above, each with its words;
  the 17th pointer element; `pointer.*` among a shader's inputs and the 16-input cap.
- `gui.panel` gains POINTER, over a real processor: a press on an `area` writes `x` and `y` at the press, a drag
  follows, the release ends one host gesture (one begin, one end a param; the journal one change); `down` goes
  high then low; `drag=relative` moves by travel and Shift slows it; `release=spring`; the wheel nudges;
  double-click resets; `hover=` writes a panel dial and makes no host gesture, and one naming a host parameter
  writes nothing; a knob inside a `box` with pointer properties still turns and the box gets the press beside
  it; the keys; the accessibility items; the dial menu and a learned MIDI control on an `area`'s param; the
  program's param really moves (the Engine's dials).
- `gui.shaders` gains POINTER: a shader's `pointer.*` inputs follow the pointer; it draws on pointer events and
  not between them (the frame counts); a shader with `y=` writes its param when dragged.
- `plugin.sandbox`: `render` with `values` of `pointer.hover` and `pointer.down` changes the picture of the
  prompt's shader dial (GPU rows skipped where there are no GPU pictures).
- `plugin.chatexample`, `plugin.mcp` (the guide's token bound and the budget), `fminus.examples` for the two new
  examples, `gui.midi` (the dial menu on a pointer element).
- PNGs: `gui.panel.pointer*.png` (an area over a display; a shader dial at rest, hovered, pressed), both themes.

## 6. Docs

README, "The interface": pointer properties, `area`, the pointer in a shader, with the example. CHANGELOG:
Added. CLAUDE.md: the `.gui` recipe gains the pointer's rules (one press one gesture; `hover=` panel-only;
children take presses first; a shader's pointer inputs are input, not motion); `GuiSpec`'s and `GuiItems`'
entries; the extension's version.

## 7. Risks and what was left out

- **Events into the program and click actions were not chosen** (D2). What they would have given is reached
  through params: a `down=` param is a trigger the program reads; a step grid is sixteen bool params, which fits
  the 64 dials but spends them. If grids matter later, the next step is a `cells` element over an int param used
  as a bit mask, not a new call into programs.
- **A program with fewer than 33 params has no panel-only param**, so `hover=` has nothing to name; shaders'
  `pointer.hover` covers the visual case, which is nearly every case. If hover into a program turns out to
  matter, the fix is a param attribute that makes a param panel-only wherever it is declared (a change to the
  slot mapping in `Controls.h` and to the contract: its own package).
- **Host parameters written by a drag on a picture** look like any automation to the host; that is the intent,
  but a panel that writes many params at once on one press makes one gesture of all of them: keep `tapMany`'s
  batching so a host sees them begin and end together.
