# 01. One scrolling behaviour, the code editor's

Planning-document items: "Explorer: fix scrolling behavior to match the Code Editor's vertical scrolling behavior"
and "Project/preset popup: make vertical scrolling match the Code Editor's". Size M. Needs nothing.

## 1. What the user gets

Every list in the editor scrolls as the code does: the same direction, the same distance for the same movement of
the wheel or the trackpad, to the pixel, and a thumb that can be dragged.

## 2. What exists

The reference is FunkGui's `TextEditor::wheel` (`src/widgets/TextEditor.cpp`, constants at its top):

```
const float px = e.smooth ? kWheelPxPerUnit : kRowsPerNotch * rowH_ / kWheelNotch;   // 512 ; 3 rows a notch
scrollY_ -= e.dy * px; scrollX_ -= e.dx * px; clampScroll();
```

`kWheelPxPerUnit = 512` (one px for each point of a trackpad), `kWheelNotch = 50/256`, `kRowsPerNotch = 3`, bar
`kBarW = 4`, inset 2, least thumb 12. The offset is a float, clamped to `[0, content - view]`, never animated. It
**ignores `reversed`, `inertial` and the modifiers**. Its thumb drags, and a press on the track puts the thumb
under the pointer.

**Ignoring `reversed` is right, and flipping on it is the fault** (confirmed from JUCE 8.0.4's source on
2026-11-12; not yet felt on a Mac):

- macOS (`juce_gui_basics/native/juce_NSViewComponentPeer_mac.mm`, the wheel handler): `wheel.isReversed = [ev
  isDirectionInvertedFromDevice];` and `wheel.deltaY = scale * (float) [ev scrollingDeltaY];`. AppKit's
  `scrollingDeltaY` already has the user's natural-scrolling setting applied; `isReversed` only reports that it was.
- `mouse/juce_MouseEvent.h` says the same of `deltaY`: with `isReversed`, "a negative deltaY means that the wheel
  has been pushed physically upwards". The flag describes the physical gesture, not the direction to scroll.
- Windows and Linux (`juce_Windowing_windows.cpp`, `juce_XWindowSystem_linux.cpp`): `wheel.isReversed = false;`
  always. So a flip changes nothing there, and everything on a Mac with natural scrolling on, the default.
- JUCE's own `Viewport` (`layout/juce_Viewport.cpp`) scrolls by `pos.y -= deltaY` and never reads the flag. Only
  `widgets/juce_Slider.cpp` reads it, because a slider maps the physical direction onto a value.
- FunkGui's `EditorHost` (and `nogpu/SoftEditorHost` here) copies JUCE's fields straight through. Its `Dial` and
  `RuleSlider` flip on `reversed`, as JUCE's Slider does: right, for a value.

So the rule is: **what moves content never reads `reversed`; what sets a value or zooms may** (the physical
direction, as JUCE's Slider and Blender's zoom take it).

Every other surface has its own copy of the wheel's rule (all in `Source/editor/`):

| Surface | Today | Differs by |
|---|---|---|
| Explorer (`Explorer.cpp`, `wheel`) | `smooth ? dy*120 : dy*40*3`, float offset; flips on `reversed` | direction with natural scrolling; a trackpad about 4.3 times slower; a notch about one row; no thumb drag |
| Projects card, three lists (`ProjectBrowser::wheel`) | integer first row, `max(1, round(abs(dy)*6))` rows; flips on `reversed`; ignores `smooth` | direction; whole rows; a trackpad moves a row an event, so it races |
| Preset picker (`PresetPicker.cpp`) | the same whole-row rule, and no test of where the pointer is | the same |
| A/B card (`AbCard.cpp`) | list: whole rows; diff: 3 lines a wheel event | whole rows |
| Settings card (`SettingsCard.cpp`) | `smooth ? 512 : 292` px a unit; flips on `reversed` | direction; a notch is 57 px whatever the row |
| Chat (`ChatView.cpp`) | 512 / 292, `reversed` ignored | a notch's distance only; no thumb drag |
| `.gui` panel (`Panel::wheel`, then `scrollGui` and `GuiView::scrollBy`) | 512 / 292; **flips on `reversed`**, both axes, in the Panel's routing | direction; a notch's distance; no thumb drag |
| Schematic (`SchematicView::wheel`) | the probe lanes' scroll and the canvas's pan and zoom all take a delta **flipped on `reversed`** | direction of the lanes and of the pan (the zoom's flip is right: below) |

None but the TextEditor has a thumb that drags. There is no shared helper.

A `.gui` control's own wheel (`GuiControlItem::wheel`, `GuiItems.cpp`) also flips on `reversed`. It sets a value,
so it is right and stays.

## 3. Design

One helper, `Source/editor/Scroll.h` and `.cpp` (in CMake's `FP_SANDBOX_SHARED`, as `GuiView` will use it):

```cpp
namespace ui
{
    // One axis of scrolling, as funkgui::TextEditor scrolls: a float offset in px, the wheel's rule, a thumb.
    struct Scroll
    {
        float offset = 0.0f;                 // 0 .. max(), never rounded here
        float content = 0.0f, view = 0.0f;   // set by the owner at each layout
        float row = 24.0f;                   // the height three of which a notch moves

        float max() const noexcept;
        void  clamp() noexcept;
        bool  wheel(float delta, bool smooth) noexcept;      // false when nothing overflows; an edge bump is true
        void  reveal(float from, float to) noexcept;         // bring a row into view (the keys)
        bool  shown() const noexcept;                        // content > view
        funkgui::Rect thumb(const funkgui::Rect& area) const noexcept;
        bool  press(funkgui::Point p, const funkgui::Rect& area) noexcept;   // thumb: drag; track: jump, then drag
        void  drag(funkgui::Point p, const funkgui::Rect& area) noexcept;
        void  release() noexcept;
        bool  dragging() const noexcept;
        void  draw(funkgui::Canvas& c, const funkgui::Rect& area, const funkgui::Theme& th) const;
        uint64_t key() const noexcept;                       // for the owner's drawKey()
    };
}
```

- The rule is the TextEditor's exactly: `offset -= delta * (smooth ? 512 : 3 * row / (50/256))`. The caller passes
  `e.dy` (or `e.dx`) as it came: **`reversed` is never read** by anything that moves content. A value control's
  wheel and the schematic's zoom keep their flip (section 2).
- The bar is the TextEditor's: 4 px wide, 2 px in, a thumb at least 12 px, `ink32`, `ink52` while dragged, hit
  through a rect 2 px larger.
- The constants are copied from FunkGui until FunkGui exposes them (package 10, F1, `funkgui/widgets/Scroll.h`);
  then this file includes that header and the copies go. Until then a probe row holds them to the TextEditor
  (section 5).
- A list of rows draws partial rows now, so each list clips its area (`pushClip`) and hit-tests by px. Rows keep
  their heights.

## 4. Work

1. **The direction fault: confirmed** from JUCE's source (section 2), so the flips go. What is still owed is one
   look on a Mac with natural scrolling on: the explorer, the Projects card, a `.gui` panel that scrolls and the
   schematic's pan, each against a code tab, before and after.
2. `Scroll.h`/`.cpp`, with the rule, the thumb and its drag.
3. Convert, one commit each, keeping every list's keys (arrows, Page, Home, End move the selection and `reveal`):
   - `Explorer.cpp`: its float offset becomes a `Scroll`; the thumb takes presses before the rows.
   - `ProjectBrowser.cpp`: `first_[3]` (integer rows) becomes three `Scroll`s; `places()` no longer shortens the
     row count by the first row, rows are drawn from `floor(offset / rowH)` with a clip; `wheel` keeps choosing
     the list under the pointer. A drag of projects onto a group must not start on the thumb.
   - `PresetPicker.cpp`: the same, and the wheel acts only over its list.
   - `AbCard.cpp`: the versions list and the diff (the diff moves into `DiffView` in package 03: do 01 first, or
     convert it there).
   - `SettingsCard.cpp`: drop the `reversed` flip; a `Scroll` with `row` its line pitch.
   - `ChatView.cpp` and `GuiView.cpp`: keep 512 for a trackpad; a notch becomes `3 * row` with `row` the body
     line's height (chat) or 24 px (a panel), in place of the bare 292; take the thumb's drag. `GuiView` still
     places its items at whole px (`round(offset)`): keep that. The `.gui` panel's flip is in `Panel::wheel`
     (the two deltas made before `scrollGui`): take it out there.
   - `SchematicView.cpp`, the direction only: the probe lanes' scroll and both of the canvas's pans (`panBy`) take
     the delta as delivered; the zoom keeps its flip, with a line saying why (the physical direction). The lanes
     keep their step, and nothing else of the schematic's navigation changes here: that is package 11.
4. `Panel.cpp`: route presses, drags and releases on a thumb to its owner (the cards already get every event
   while open); mix each `Scroll::key()` into the owner's `drawKey`.
5. The completion list (`CodeAssist.cpp`, whole rows) stays as it is: it is not a list the brief names, and whole
   rows suit a popup of fixed rows. A `.gui` control's wheel keeps its flip (a value).

## 5. Tests

- New probe `gui.scroll`: for each converted surface and a code tab, the same `WheelEvent` moves the same px
  (smooth: `dy * 512`; a notch: three of that surface's rows); with `reversed` set the direction is the code tab's;
  the offset stops at both ends; a thumb drag moves the content in proportion; a press on the track jumps; a list
  that fits has no thumb and takes no wheel. One row holds this file's constants to the TextEditor's behaviour (a
  code tab scrolled by one notch moves `3 * rowHeight()`).
  HeadlessHost's `wheel()` cannot set `reversed` or `dx` before F1: until then those rows call the item's `wheel`
  with a built `WheelEvent` (the Panel's test hooks), and say so.
- Direction rows for the two surfaces that only lose their flip: a `.gui` panel that scrolls moves the same way
  with `reversed` set as without (`gui.panel` SCROLL or `gui.scroll`); the schematic's pan and its probe lanes
  likewise, while its zoom still inverts with `reversed` (`gui.schematic`); a `.gui` knob's wheel still inverts.
- Existing rows that encode whole rows change with their surface: `gui.projects` (`projects.cpp`, the wheel near
  line 146), `gui.dump` (near 1587, "1/6-unit steps"), `gui.ab` SCROLL, `gui.explorer` (near 595), `gui.settings`
  SCROLL, `gui.presets` LIST.
- Look at `gui.projects*.png`, `gui.explorer*.png`, `gui.presets*.png`, `gui.settings*.png`: a half row at a
  list's edge is now right.

## 6. Docs

README, "The editor": one sentence where the explorer and the Projects card are described (they scroll as the
code does, the thumb drags). CHANGELOG under Fixed (on a Mac with natural scrolling the lists, a `.gui` panel and
the schematic's pan now move the way the code does). CLAUDE.md: `Scroll` in the `Source/editor/**` row, and one
rule: a new scrolling surface uses `ui::Scroll`; what moves content never reads `WheelEvent::reversed`, and only a
value control's wheel or a zoom may.

## 7. Risks

- The fault is confirmed from source, not on hardware (work step 1). If a Mac shows otherwise, the rule moves into
  FunkGui's hosts (normalise the delta there once) rather than into each surface. Either way one place decides.
- Partial rows change every list's hit-testing; the drag-to-group gesture in the Projects card is the one to
  re-check by hand in the PNGs and the DRAG section.
