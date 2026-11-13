Claude Desktop extension: **0.20.0** (was 0.19.0).

### The pointer drives a panel

**Added**
- A panel can have controls of its own making. In a `.gui` file, an `area` (a new element that draws nothing), a
  `shader`, a row, column, grid, section, box, label or display can name parameters the pointer writes over it: `x=`
  and `y=` (where it is, across and up), `down=` (while it is pressed), `wheel=` and `hover=` (while it is over it: one
  of the panel's own dials). `drag=relative` moves by the pointer's travel instead, `release=spring` puts `x` and `y`
  back when let go, and `cursor=` shapes the pointer. One press is one gesture: one automation pass in the host, one
  undo step. A knob inside such a box still turns, and a display inside it passes the press on.
- A shader can read the pointer over it as inputs, `pointer.x`, `pointer.y`, `pointer.down` and `pointer.hover`, so a
  control can be drawn entirely by a shader and dragged like a knob; it draws again only when they change.
- Such an element is a Tab stop: the arrows move it, Home puts it back, Space presses it. A screen reader sees a
  slider for each of `x` and `y` and a button for `down`. A right-click opens the first parameter's menu, MIDI learn
  included.
- Two examples drawn this way: Light filter (the effect: a shader dial for the cutoff, and a spectrum to drag it
  across) and Light saw (the synth: a shader dial for the brightness, and a scope to hold down for a swell). An example
  can now be a folder, its program with its `panel.gui` and shader beside it.
- Claude is taught the pointer, with a shader dial as its example, and view_panel and the GUI sandbox draw a panel as
  if the pointer were over its shaders or pressing them (`values` naming `pointer.hover`, `pointer.down`, `pointer.x`,
  `pointer.y`).

**Changed**
- A shader's `width=` and `height=` are now its size. Before, a shader was never smaller than 200 x 120 px, whatever
  they said: a panel written with `shader "dash.sc" width=170 height=110` drew it 200 x 120, and now draws it 170 x 110,
  as written. To keep the old look, give it `width=200 height=120` (or leave both out: 200 x 120 as before).
