### The schematic moves around as Blender's Node Editor does

**Changed**
- A netlist's Schematic view: **Ctrl-wheel and Shift-wheel now pan** (sideways, and up and down, three grid steps a
  notch) where they zoomed; the plain wheel still zooms toward the pointer (1.25 a notch). A trackpad pans both ways
  whatever Shift does, and Ctrl or Cmd with two fingers zooms. A trackpad's stroke no longer zooms for a second after a
  zoom: its momentum does what the stroke did, and the next stroke is read afresh.
- **Cmd-=**, **Cmd--** and **Cmd-0** in the Schematic view zoom about the pointer when it is over the drawing.
- A middle-button press anywhere but the Schematic view's drawing does nothing (it used to click).

**Added**
- In the Schematic view, a middle-button drag pans, a Ctrl- (or Cmd-) middle-button drag zooms about where it pressed,
  and a trackpad's pinch zooms toward the pointer.
- **.** frames the selection (with nothing selected, everything), and the canvas's menu has **Frame selection**.
- The settings' Debug page names the FunkGui compiled in, and while the GPU runs its renderer, vendor and device, its
  largest texture and the custom shaders registered.

**Fixed**
- **F**, Home and the menu's Fit keep the view where the easing lands in the session, not where it started.
