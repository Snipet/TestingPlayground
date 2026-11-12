# 11. Circuit editor navigation as Blender's Node Editor

Planning-document item: "Redo mouse and trackpad navigation to match Blender's Node Editor's UX." Size M. Needs
FunkGui's F1 (package 10: the pointer's button, the pinch, a whole wheel event in HeadlessHost) and the pin move.

## 1. Blender's Node Editor, the reference

From Blender's manual (Node Editors, Navigating) and its View2D keymap:

| Blender | Does |
|---|---|
| Middle-button drag | pans |
| Wheel | zooms, toward the pointer |
| Ctrl-wheel | pans sideways |
| Shift-wheel | pans up and down |
| Ctrl-middle-button drag | zooms |
| Trackpad, two fingers | pans, both ways at once |
| Trackpad pinch, or Ctrl with two fingers | zooms, toward the pointer |
| Home | frames everything |
| Numpad `.` | frames the selection |
| Alt-left-drag | pans (its "Emulate 3 Button Mouse", for a laptop) |
| Left-drag on empty space | box select |
| A node dragged to the edge | the view follows |

## 2. What exists (`Source/editor/SchematicView.h/.cpp`)

- **The wheel** (`SchematicView::wheel`):
  ```
  const bool zooms = !e.smooth || e.mods.cmd || e.mods.ctrl || nowSec - zoomedAt_ < 1.0;
  ```
  A notched wheel always zooms toward the pointer (about 1.31 a notch), whatever key is held. A trackpad pans one
  to one; Shift makes it sideways; Cmd or Ctrl makes it zoom. For a second after any zoom, a trackpad's pan
  zooms too (`zoomedAt_`), which fights panning.
  The handler flips its deltas on `WheelEvent::reversed` for the zoom, the pan and the probe lanes alike. Package
  01 takes the flip off the pan and the lanes (content follows the delta as delivered) and leaves it on the zoom,
  which maps the physical direction, as Blender's does. This package keeps both.
- **Drags** (`beginDrag`): a right-button drag pans, and so does Alt-left-drag (`press_.popup || press_.mods.alt`).
  **A middle press arrives as a left one** (FunkGui gives no button), so it selects and moves parts.
- No pinch (FunkGui has no such event).
- **Keys:** Home and F fit; Cmd-0 is 100 %; Cmd-= and Cmd-- zoom about the centre; the arrows nudge the
  selection; Space is swallowed. No frame-selected.
- Zoom 25 % to 400 % (`kMinZoom`, `kMaxZoom`), `zoomTo(z, about)` keeps the point under `about`; `panBy`; no
  limit on the pan; only `fit()` eases. Edge auto-pan while moving, wiring, box-selecting (`kEdgePx = 16`).
- Left-drag on empty space is the box select already. Alt-click pins a probe.
- The view (zoom, centre) is kept in the session on the user's gestures (`persist()`, `EditorPort::fileViews`).
- `gui.schematic` checks the zoom's limits, the right-drag and Alt-drag pans, the edge pan, persistence. It has
  no wheel row at all.
- Off the Mac, JUCE's Ctrl sets both `cmd` and `ctrl`.

## 3. The mapping

| Input | Today | After |
|---|---|---|
| Middle-button drag | acts as a left press | **pans** (the grabbing hand) |
| Ctrl-middle-button drag | the same | **zooms** about the point pressed: up or right in, down or left out, a factor of 2 every 120 px |
| Wheel, a notch | zooms toward the pointer | the same (1.25 a notch, `kStepZoom`) |
| Ctrl- or Cmd-wheel, a notch | zooms | **pans sideways**, 3 grid units a notch |
| Shift-wheel, a notch | zooms | **pans up and down**, 3 grid units a notch |
| Trackpad, two fingers | pans (Shift: sideways only) | pans both ways; Shift no longer changes it |
| Ctrl- or Cmd- two fingers | zooms toward the pointer | the same |
| Pinch | nothing | **zooms toward the pointer**, by the gesture's own factor |
| After a zoom, for 1 s | a trackpad's pan zooms | **gone.** Momentum (`inertial`) events keep doing what the gesture they belong to did; a new gesture is read afresh |
| Home | fits | frames everything (the same) |
| `.` | nothing | **frames the selection** (nothing selected: everything), eased as Home |
| F | fits | kept |
| Alt-left-drag | pans | kept: Blender's own laptop way |
| Right-button drag | pans | kept as an extra (assumption A8); a right click with no drag still opens the menu |
| Arrows | nudge the selection | kept (Blender moves nodes with G; not this item's business) |
| Cmd-0, Cmd-=, Cmd-- | 100 %, in, out, about the centre | kept, about the pointer when it is over the drawing |
| Left-drag on empty space | box select | kept |
| Edge auto-pan | yes | kept |

`smooth` is what tells a trackpad (or a Magic Mouse) from a notched wheel, as today; a precise mouse wheel that
reports `smooth` behaves as a trackpad, which is Blender's behaviour for such devices too.

## 4. Work

1. **The Panel routes buttons** (`Panel::pointerDown`, `pointerDrag`, `pointerUp`): a press whose `button` is
   not left reaches only an item that asks for it (`Item::takesButtons()`, false by default; the schematic and,
   later, package 13's scenes say true). Everywhere else a middle press now does nothing, where today it clicks.
   `Panel::magnify` goes to the item under the pointer.
2. **`SchematicView`:**
   - `beginDrag`: middle is `Drag::pan`; Ctrl-middle is a new `Drag::zoom` (the anchor is the press point);
     right and Alt-left as now.
   - `wheel`: the table above. The mode of a gesture (pan or zoom) is decided at its first non-inertial event and
     kept for its inertial tail; `zoomedAt_` goes. Every pan takes the delta as delivered and every zoom keeps the
     flip on `reversed` (package 01's rule), the new Ctrl- and Shift-wheel pans and the pinch included.
   - `magnify`: `zoomTo(zoom() * e.scale, {e.x, e.y})`, then `persist()`.
   - `.`: `frameSelection()`, sharing `fit()`'s arithmetic over the selection's bounds (8 units of margin, the
     zoom never above 200 % for a single part), eased.
   - The canvas menu gains "Frame selection" beside "Fit"; the toolbar's zoom cell is unchanged.
   - The cursor is `grabbing` during a middle pan, `upDown` during a zoom drag.
   - The probe card and the probe lanes keep taking the wheel when the pointer is over them.
3. **`nogpu/SoftEditorHost`** sends buttons and the pinch as FunkGui's `EditorHost` does (after F1).
4. The schematic's help line and the README's key table.

## 5. Tests

`gui.schematic` gains NAV, through HeadlessHost's new calls (F1):

- a notch zooms by 1.25 and the world point under the pointer stays under it; at 25 % and 400 % it stops;
- Ctrl-notch moves the centre sideways only, Shift-notch up and down only, neither changes the zoom;
- a smooth wheel pans by its px both ways; with Ctrl it zooms; Shift changes nothing;
- after a zoom, a smooth wheel pans at once (no guard); an inertial tail after a Ctrl-zoom whose Ctrl was let go
  keeps zooming, and the next gesture pans;
- a pinch of 1.5 multiplies the zoom by 1.5 about the pointer;
- a middle drag pans by the pointer's movement, selects nothing and moves no part, with a part under the press
  too; Ctrl-middle drag zooms about the press;
- a middle press on the code editor, a dial, a tab and the explorer does nothing;
- `.` frames a selection of two parts (both in view, the margin), and everything when nothing is selected;
- every gesture above is kept in the session (`fileViews`) and none writes the netlist's text (the tab's
  revision does not move).

The rows that exist for right-drag and Alt-drag panning stay. PNG: none new (navigation moves the view; the
existing pictures at 25 % and 400 % stand).

## 6. Docs

README, "The schematic": the navigation table, in Blender's words. CHANGELOG: Changed (a wheel with Ctrl or Shift
now pans; a middle button pans; pinch zooms; `.` frames the selection). CLAUDE.md: `SchematicView`'s entry gains
the navigation's rule (the table is Blender's; a gesture's mode is decided at its start; no call is cut and no
text written for a view change), and the Panel's button routing (`takesButtons`).

## 7. Risks

- **A changed habit.** Today Ctrl-wheel and Shift-wheel zoom with a notched mouse; after this they pan. That is
  the point of the item, and the CHANGELOG says it first.
- **Hosts that eat gestures.** Some DAWs take the pinch, the middle button or Ctrl-wheel for themselves before a
  plugin sees them. Nothing can be done from inside; the wheel, Alt-drag and the keys always work, and the README
  says so.
- **No numpad in FunkGui's keys**, so `.` is the main keyboard's and the numpad's alike. If a text field in the
  schematic has the caret (a part's value being edited), `.` types, as every letter does there.
