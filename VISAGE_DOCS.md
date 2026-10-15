# Visage Claude Code Skill

A Claude Code skill that teaches Claude how to use the **Visage** C++ graphics library (by Vital Audio) to build audio plugin GUIs.

## How to install

The skill is a folder with `SKILL.md` plus three reference files. To install for personal use, recreate this structure under `~/.claude/skills/visage/`:

```
~/.claude/skills/visage/
├── SKILL.md
└── references/
    ├── api-cheatsheet.md
    ├── plugin-integration.md
    └── build-and-cmake.md
```

The fastest way is to paste this into Terminal:

```bash
mkdir -p ~/.claude/skills/visage/references && cd ~/.claude/skills/visage
```

Then copy each of the four sections below into its own file, exactly as shown. Each section starts with `===== FILE: ...path... =====` — that header is the path the content goes in (relative to `~/.claude/skills/visage/`).

Once installed, Claude Code will pick up the skill automatically the next time it starts. You can confirm it's loaded by asking Claude "what skills do you have?" — `visage` should appear.

---

===== FILE: SKILL.md =====

```markdown
---
name: visage
description: Build audio plugin GUIs with Visage, the GPU-accelerated C++17 UI library from Vital Audio (makers of the Vital synth). Use this skill whenever the user is writing or reviewing C++ code that touches Visage — including `visage::Frame`, `visage::Canvas`, `visage::ApplicationWindow`, `VISAGE_THEME_COLOR`, custom bgfx shaders for a plugin UI, or any repo that depends on github.com/VitalAudio/visage. Also trigger when the user is building a CLAP/VST3/AU/iPlug2/JUCE plugin and asks about GPU-accelerated rendering, custom shader UIs, or embedding a Visage editor inside a host-provided HWND/NSView/X11 window — even if they don't say the word "Visage." Covers the Frame component tree, the Canvas drawing API, flex layout, theme system, animation helpers, post-effects, the CMake/FetchContent build setup, and the canonical CLAP-style plugin embedding pattern.
---

# Visage (Vital Audio)

Visage is a GPU-accelerated, cross-platform C++17 UI library from Vital Audio. It ships a retained-mode UI framework (a `Frame` tree with lifecycle, events, and flex layout) plus a creative-coding-style 2D canvas (paths, shaders, gradients, post-effects, blend modes, bloom, blur). Its niche is **native plugin UI plus shader art**: it competes more with JUCE's `juce::Graphics` module than with all of JUCE. It is **not an audio framework** — there is no DSP, MIDI, parameter, or plugin-host code. The author has to bring their own plugin framework (CLAP, VST3, AU, iPlug2, JUCE without its GUI, DPF, etc.) and embed Visage as the renderer.

Backend: **bgfx** (Direct3D 11 on Windows, Metal on macOS, Vulkan on Linux, WebGL on Emscripten), pinned and fetched automatically at CMake configure time. **License: MIT.** **Wayland is unsupported** — Linux is X11 only.

Repository: https://github.com/VitalAudio/visage

## When this skill should pull its weight

This skill is meant for substantive work: writing a custom plugin editor, designing a Visage component tree, picking the right embedding mode for a host, debugging a redraw or layout problem, or writing CMake glue. For a one-line "what is Visage?" question, just answer from the overview above — no need to load the references.

## Mental model — read this before writing any code

Five ideas carry most of the library. Get these straight and the rest reads naturally.

**1. `Frame` is to Visage what `Component` is to JUCE.** It's a retained-mode tree node with virtuals — `draw(Canvas&)`, `resized()`, `mouseDown/Move/Up/Drag/Wheel`, `keyPress/Release`, etc. Every virtual has a matching `onXxx()` callback list, so two styles coexist: subclass and override, or instantiate and attach lambdas. Headers: `visage_ui/frame.h`. Both styles appear throughout the examples — pick whichever fits the situation; don't mix them for the same event on the same frame.

**2. `ApplicationWindow` is your top-level Frame and your embedding seam.** It is a `Frame` that owns a native `Window` (HWND / NSView / X11 Window). Two embedding modes:

- Standalone app: `app.show(width, height); app.runEventLoop();`
- Plugin (child of host's native window): `app.show(width, height, parent_handle);` — **do not** call `runEventLoop()`; the host owns the message loop.

Header: `visage_app/application_window.h`. This is the *only* surface where plugin integration happens — every audio-plugin question routes through this class.

**3. `Canvas` is an immediate-mode-style API over a retained, batched renderer.** Inside `draw(Canvas& canvas)`, calls like `canvas.setColor(0xffaabbcc); canvas.roundedRectangle(x, y, w, h, rounding);` *look* immediate, but the canvas batches shapes per region and submits to bgfx in one pass. Coordinates inside `draw` are **local to the frame** — `(0, 0)` is the frame's top-left. The frame only repaints when it's marked dirty, so **you must call `redraw()`** after state changes that should be visible. Header: `visage_graphics/canvas.h`.

**4. Dimensions are typed and come in three flavors.** `Dimension` accepts:
- Logical (DPI-aware) pixels: `100_px`
- Native (device) pixels: `100_npx`
- Viewport units: `50_vw`, `50_vh`, `50_vmin`, `50_vmax`

UDLs live in `namespace visage::dimension`. Add `using namespace visage::dimension;` near layout/show calls. Most layout setters require `Dimension` — passing a bare `int` to `layout().setWidth(100)` works because `int` → `Dimension(int, Native)` (i.e., native pixels), which is rarely what you want for a multi-platform plugin UI.

**5. Visage uses flex layout, not absolute positioning, for anything non-trivial.** Each `Frame` has a lazy `Layout` accessor: `frame.layout().setFlex(true); frame.layout().setFlexGap(8_px);`. Conceptually it's CSS flexbox — `setFlexRows`, `setFlexGrow`, `setFlexWrap`, `setFlexItemAlignment`. For prototypes you can also call `setBounds(x, y, w, h)` directly inside `resized()` (the JUCE pattern), but stick with flex for production GUIs because plugin windows resize.

## The two embedding modes (pick one before writing code)

**Standalone application** — easiest for prototyping. The window owns its own native handle and message loop. `ApplicationWindow::runEventLoop()` blocks until the user closes it. Use this for examples, dev tools, and when validating a UI design before wiring it into a plugin.

**Plugin embedded in host window** — the host (CLAP/VST3/AU/iPlug2 wrapper) hands you a parent native handle and drives its own event loop. The contract:

| Plugin lifecycle event | What to do with Visage |
|---|---|
| Host calls `guiCreate` (or equivalent) | Construct `std::unique_ptr<visage::ApplicationWindow>` |
| Host calls `guiSetParent(parent_handle)` | `app_->show(width, height, parent_handle->ptr)` |
| Host queries size | `app_->nativeWidth()` / `app_->nativeHeight()` on Win/Linux; `app_->width()` / `app_->height()` on macOS (logical points) |
| Host commands resize | `setNativeWindowDimensions(w, h)` on Win/Linux; `setWindowDimensions(w_px, h_px)` on macOS |
| Editor wants to resize itself | Set `app_->onWindowContentsResized()` callback; call `host.guiRequestResize(...)` from inside |
| Linux X11 event integration | Register `app_->window()->posixFd()` with the host's POSIX fd support; in the callback invoke `app_->window()->processPluginFdEvents()` |
| Host calls `guiDestroy` | `app_->close(); app_.reset();` |

The macOS-vs-Windows divergence (logical points vs native pixels) is a real bug surface — read `references/plugin-integration.md` before shipping a cross-platform plugin.

The repo ships one first-party host example: `examples/ClapPlugin/`. There is **no first-party VST3, AU, iPlug2, JUCE, or DPF example**. The patterns map (every host gives you a parent handle and a resize protocol), but specifics — AU's NSAutoreleasePool requirements, JUCE's message-thread expectations, VST3's `IPlugView` semantics — must be invented by the integrator and tested. Be honest with the user about that gap; don't fabricate signatures that don't exist.

## Canonical patterns

These are quoted from the actual examples in the repo (with file references). Use them as starting points; for the full API surface, see `references/api-cheatsheet.md`.

### Minimal standalone app (callback style)

\`\`\`cpp
#include <visage/app.h>

int main() {
  visage::ApplicationWindow app;
  app.onDraw() = [&app](visage::Canvas& canvas) {
    canvas.setColor(0xff000066);
    canvas.fill(0, 0, app.width(), app.height());

    float r = app.height() * 0.1f;
    canvas.setColor(0xff00ffff);
    canvas.circle(app.width() * 0.5f - r, app.height() * 0.5f - r, 2.0f * r);
  };
  app.setTitle("My Visage App");
  app.show(800, 600);
  app.runEventLoop();   // blocks; only valid in standalone mode
}
\`\`\`
Source: `examples/Basic/basic.cpp`.

### Subclass style with mouse handling

\`\`\`cpp
class MyEditor : public visage::ApplicationWindow {
public:
  void draw(visage::Canvas& canvas) override {
    canvas.setColor(0xff000000);
    canvas.fill(0, 0, width(), height());
    canvas.setColor(0xffffaa00);
    canvas.circle(cursor_.x - 8, cursor_.y - 8, 16);
  }
  void mouseMove(const visage::MouseEvent& e) override {
    cursor_ = e.position;     // frame-local coordinates
    redraw();                 // partial redraw — required
  }
private:
  visage::Point cursor_{};
};
\`\`\`
Source: composed from `examples/MouseEvents/mouse_events.cpp`. The key thing: **`redraw()` is mandatory after state changes that should be visible**, because Visage is a dirty-region renderer.

### Flex layout with children

\`\`\`cpp
#include <visage/app.h>
using namespace visage::dimension;

class Grid : public visage::ApplicationWindow {
public:
  Grid() {
    setFlexLayout(true);
    layout().setPadding(10_px);
    layout().setFlexGap(10_px);
    layout().setFlexWrap(true);

    for (visage::Frame& f : tiles_) {
      addChild(&f);
      f.layout().setWidth(100_px);
      f.layout().setHeight(100_px);
      f.layout().setFlexGrow(1.0f);
      f.onDraw() = [&f](visage::Canvas& c) {
        c.setColor(0xff444444);
        c.roundedRectangle(0, 0, f.width(), f.height(), 12);
      };
    }
  }
private:
  visage::Frame tiles_[12];
};
\`\`\`
Source: pattern from `examples/Layout/layout.cpp`.

### Animating a hover state

\`\`\`cpp
class HoverBox : public visage::Frame {
public:
  HoverBox() {
    hover_.setSourceValue(0.0f);
    hover_.setTargetValue(1.0f);
    hover_.setAnimationTime(120);   // ms
  }
  void mouseEnter(const visage::MouseEvent&) override { hover_.target(true);  redraw(); }
  void mouseExit (const visage::MouseEvent&) override { hover_.target(false); redraw(); }

  void draw(visage::Canvas& canvas) override {
    float t = hover_.update();    // call once per frame; returns eased value
    canvas.setColor(visage::Brush::interpolate(
        visage::Brush::solid(0xff333333),
        visage::Brush::solid(0xffff8800), t));
    canvas.roundedRectangle(0, 0, width(), height(), 8);
    if (hover_.isAnimating()) redraw();   // keep ticking until eased value settles
  }
private:
  visage::Animation<float> hover_;
};
\`\`\`
`Animation<T>` is a value-smoother, not a tween scheduler — the user drives it by calling `update()` from `draw()` and re-issuing `redraw()` while `isAnimating()` is true. Header: `visage_graphics/animation.h`.

### Embedding in a CLAP host (abridged)

\`\`\`cpp
bool ClapPlugin::guiCreate(const char* /*api*/, bool /*is_floating*/) noexcept {
  using namespace visage::dimension;
  app_ = std::make_unique<visage::ApplicationWindow>();
  app_->setWindowDimensions(80_vmin, 60_vmin);

  app_->onDraw() = [this](visage::Canvas& c) {
    c.setColor(0xff111122);
    c.fill(0, 0, app_->width(), app_->height());
  };

  // bubble editor-driven resizes up to the host
  app_->onWindowContentsResized() = [this] {
    _host.guiRequestResize(app_->nativeWidth(), app_->nativeHeight());
  };
  return true;
}

bool ClapPlugin::guiSetParent(const clap_window* window) noexcept {
  app_->show(window->ptr);    // parent_handle: HWND / NSView* / X11 Window
#if __linux__
  if (_host.canUsePosixFdSupport() && app_->window()) {
    int flags = CLAP_POSIX_FD_READ | CLAP_POSIX_FD_WRITE | CLAP_POSIX_FD_ERROR;
    return _host.posixFdSupportRegister(app_->window()->posixFd(), flags);
  }
#endif
  return true;
}

void ClapPlugin::guiDestroy() noexcept { app_->close(); app_.reset(); }
\`\`\`
Source: condensed from `examples/ClapPlugin/clap_plugin.cpp`. For full lifecycle (resize hints, aspect ratio, the macOS-vs-Win/Linux pixel-space split, fixed-aspect support), read `references/plugin-integration.md`.

## What to read next

The SKILL.md above is the working surface. The reference files exist so this file stays focused.

- **`references/api-cheatsheet.md`** — full public-API surface (Frame, Canvas, Color/Brush/Gradient, theming macros, layout, animation, shaders, post-effects, widgets) with class names, method signatures, header paths. Read this whenever the user needs an API call you don't remember verbatim — the temptation to invent a signature is real, and Visage has very few comments in its headers, so guessing is risky.
- **`references/plugin-integration.md`** — complete embedding contract: macOS vs Windows pixel spaces, resize negotiation, X11 fd bridging, threading caveats, and how to adapt the CLAP pattern to VST3/AU/iPlug2/JUCE wrappers. Read this for any plugin-host work beyond the abridged CLAP example above.
- **`references/build-and-cmake.md`** — `FetchContent` vs `add_subdirectory` setup, the public target name, key CMake options, the resource-embedding helpers (`add_embedded_resources`, `visage_embed_shaders`), shader compilation pipeline, deployment-target footguns. Read this when scaffolding a project or debugging build errors.

## Gotchas worth surfacing proactively

These come up often enough that a Claude working in a Visage codebase should mention them when relevant, not wait for the user to step on them.

**Public headers are generated at configure time.** `<visage/app.h>` doesn't exist in the source tree — it's emitted by CMake into `${CMAKE_BINARY_DIR}/include/visage/`. If a fresh checkout's IDE can't find the header, the user hasn't configured yet. The fine-grained headers (`visage_ui/frame.h`, etc.) *are* in-tree and reachable.

**Logical vs native pixels diverge.** `width()/height()` return logical (DPI-aware) floats. `nativeWidth()/nativeHeight()` return integer device pixels. The CLAP example switches on `__APPLE__` for exactly this reason — macOS uses logical points for window APIs while Win/Linux use native pixels. Plugin code that ignores this difference will mis-size on Retina or HiDPI displays.

**`redraw()` is partial.** It marks one frame dirty; children/parents aren't repainted unless they're also dirty. For animation, call `redraw()` from inside `draw()` while still animating. For "redraw the world," `redrawAll()` exists but is heavy.

**`setIgnoresMouseEvents(ignore, pass_to_children)` has two booleans.** The second is whether children still receive events. Easy to misread.

**Theme IDs are globally numbered at static init.** `VISAGE_THEME_COLOR(MyName, 0xff112233)` allocates an id from a global counter the first time it's hit. Names must be unique within the linked binary; collisions are not caught at compile time.

**bgfx is downloaded by FetchContent.** Configuring without network access fails. The pin is in `visage_graphics/CMakeLists.txt` (currently `bgfx.cmake` v1.129.x).

**`VISAGE_ENABLE_BACKGROUND_GRAPHICS_THREAD` is OFF by default.** Whether it's safe in a plugin context (where the host may own the message thread) isn't documented in-repo. Don't flip it in a shipping plugin without measuring.

**No built-in audio-thread bridge.** Visage has no `juce::AudioProcessorValueTreeState` equivalent. The author is responsible for shuttling parameters from the audio thread to the UI — typical pattern is `std::atomic<float>` written by audio, polled by a `visage::EventTimer` running on the UI thread. The function `visage::runOnEventThread(std::function<void()>)` (in `visage_ui/events.h`) exists for one-shot marshalling but allocates and locks, so it's not safe to call from an RT audio callback.

## Honesty about what's not in the repo

- No VST3 / AU / iPlug2 / JUCE / DPF integration example. Patterns map from CLAP, but signatures must be verified.
- No `docs/` directory or wiki. README is short and there's almost no inline documentation in the headers. When in doubt, point the user to the example that demonstrates a pattern rather than fabricate API details.
- Wayland is explicitly off (`BGFX_WITH_WAYLAND OFF`). Linux is X11-only.
- The "floating" / undocked plugin GUI mode is not supported by the CLAP example (`guiIsApiSupported` returns `false` for `is_floating == true`).

If the user asks something the references and examples don't answer, say so plainly and suggest reading the relevant header — `visage_ui/frame.h`, `visage_graphics/canvas.h`, and `visage_app/application_window.h` cover ~80% of practical questions.
```

---

===== FILE: references/api-cheatsheet.md =====

```markdown
# Visage API cheatsheet

Reference for the public API surface. Signatures and file paths are quoted from the actual repository (https://github.com/VitalAudio/visage). When you need a method that isn't here, open the header — Visage's source is the documentation.

## Contents

1. Umbrella headers
2. Window / app entry — `ApplicationWindow`, `ApplicationEditor`, `Window`, `Renderer`
3. The component tree — `Frame`
4. Drawing — `Canvas`
5. Color, Brush, Gradient
6. Theme system
7. Input events
8. Layout (flex)
9. Animation
10. Path
11. Shaders and post-effects
12. Widgets (optional module)
13. Resource embedding (CMake helpers)

---

## 1. Umbrella headers

Public headers are **generated by CMake at configure time** into `${CMAKE_BINARY_DIR}/include/visage/`. They're simple `#include` amalgamations of every header in each subdirectory.

\`\`\`cpp
#include <visage/app.h>         // ApplicationWindow + transitively graphics, ui, utils, windowing
#include <visage/graphics.h>    // Canvas, Color, Brush, theme, shaders, post-effects
#include <visage/ui.h>          // Frame, events, layout, popup_menu, scroll_bar
#include <visage/widgets.h>     // UiButton, TextEditor, etc. (only if VISAGE_ENABLE_WIDGETS=ON)
#include <visage/windowing.h>   // Window, createWindow, createPluginWindow
#include <visage/utils.h>       // Dimension, events
\`\`\`

For most app/plugin code, `<visage/app.h>` is enough. The fine-grained headers (`visage_ui/frame.h`, `visage_graphics/canvas.h`, etc.) are also reachable directly and are useful in unity-build-sensitive consumers.

---

## 2. Window / app entry

### `visage::ApplicationWindow` — `visage_app/application_window.h`

Subclass this for a typical app or plugin GUI. It is-a `ApplicationEditor` which is-a `Frame`.

\`\`\`cpp
class ApplicationWindow : public ApplicationEditor {
public:
  // Standalone window
  void show();
  void show(const Dimension& w, const Dimension& h);
  void showMaximized();

  // Plugin / embedded mode — parent is HWND / NSView* / X11 Window
  void show(void* parent_window);
  void show(const Dimension& w, const Dimension& h, void* parent_window);

  void hide();
  void close();
  bool isShowing() const;

  void runEventLoop();                          // blocks; standalone only

  void setTitle(std::string);
  void setWindowOnTop(bool);
  void setWindowDecoration(Window::Decoration); // Native | Client | Popup
  void setNativeWindowDimensions(int w, int h);
  void setWindowDimensions(const Dimension& w, const Dimension& h);

  Window* window() const;

  // Callbacks (inherited from ApplicationEditor)
  std::function<void()>& onShow();
  std::function<void()>& onHide();
  std::function<void()>& onCloseRequested();
  std::function<void()>& onWindowContentsResized();
};
\`\`\`

### `visage::ApplicationEditor` — `visage_app/application_editor.h`

Use directly when you want to manage the native `Window` yourself.

\`\`\`cpp
void addToWindow(Window* window);
void removeFromWindow();
void setFixedAspectRatio(bool);
void setMinimumDimensions(float w, float h);
void adjustWindowDimensions(uint32_t* w, uint32_t* h, bool h_resize, bool v_resize);
Screenshot& takeScreenshot();
\`\`\`

### `visage::Window` — abstract, `visage_windowing/windowing.h`

The native window abstraction. You usually let `ApplicationWindow` create one. Direct factories:

\`\`\`cpp
std::unique_ptr<Window> createWindow(const Dimension& w, const Dimension& h,
                                     Window::Decoration = Window::Decoration::Native);
std::unique_ptr<Window> createWindow(const Dimension& x, const Dimension& y,
                                     const Dimension& w, const Dimension& h,
                                     Window::Decoration = Window::Decoration::Native);
std::unique_ptr<Window> createPluginWindow(const Dimension& w, const Dimension& h,
                                           void* parent_handle);

// Free helpers
void closeApplication();
bool isMobileDevice();
void setCursorStyle(MouseCursor);
Point cursorPosition();
void showMessageBox(/* implementation-specific */);
std::string readClipboardText();
void setClipboardText(const std::string&);
float defaultDpiScale();
\`\`\`

On the `Window` itself, important methods include `nativeHandle()`, `posixFd()` (Linux — used for plugin event-loop integration), `processPluginFdEvents()`, `clientWidth()/clientHeight()`, and `runEventLoop()`.

### `visage::Renderer` — `visage_graphics/renderer.h`

Singleton wrapper over bgfx. Almost always managed for you by `ApplicationWindow`. You only touch it directly in the "bring your own window" path.

\`\`\`cpp
static Renderer& instance();
void initialize(void* model_window, void* display);
void initializeWindowless();
\`\`\`

---

## 3. The component tree — `visage::Frame`

Header: `visage_ui/frame.h`. The base UI node. Every visible thing in a Visage app is (or contains) a `Frame`.

### Lifecycle / draw / layout

\`\`\`cpp
virtual void draw(Canvas& canvas);
virtual void resized();
virtual void init();
virtual void destroy();

void redraw();              // mark self dirty
void redrawAll();           // recursive

void setBounds(float x, float y, float w, float h);
void setBounds(Bounds);
void setNativeBounds(int x, int y, int w, int h);

Layout& layout();           // lazy-created
void setFlexLayout(bool);   // shorthand for layout().setFlex(true)

float x() const;  float y() const;
float width() const;  float height() const;
float right() const;  float bottom() const;
Bounds bounds() const;
Bounds localBounds() const;

void setVisible(bool);  bool isVisible() const;
void setOnTop(bool);
\`\`\`

### Children

\`\`\`cpp
void addChild(Frame* child, bool make_visible = true);
void addChild(Frame& child);
void addChild(std::unique_ptr<Frame> child);
void removeChild(Frame* child);
void removeAllChildren();
const std::vector<Frame*>& children() const;
Frame* parent() const;

template <typename T> T* findParent() const;
Frame* frameAtPoint(Point p);
\`\`\`

### Events (override or use `onXxx()` callback lists)

\`\`\`cpp
virtual void mouseEnter(const MouseEvent&);
virtual void mouseExit (const MouseEvent&);
virtual void mouseDown (const MouseEvent&);
virtual void mouseUp   (const MouseEvent&);
virtual void mouseMove (const MouseEvent&);
virtual void mouseDrag (const MouseEvent&);
virtual bool mouseWheel(const MouseEvent&);

virtual bool keyPress  (const KeyEvent&);
virtual bool keyRelease(const KeyEvent&);
virtual void textInput (const std::string&);
virtual bool receivesTextInput();

void setAcceptsKeystrokes(bool);
bool acceptsKeystrokes() const;
void setIgnoresMouseEvents(bool ignore, bool pass_to_children);
void setReceiveChildMouseEvents(bool);

void requestKeyboardFocus();
bool tryFocusTextReceiver();
bool focusNextTextReceiver(/* direction */);

void setCursorStyle(MouseCursor);
void setCursorVisible(bool);
void setMouseRelativeMode(bool);

// Drag & drop
virtual bool receivesDragDropFiles();
virtual void dropFiles(const std::vector<std::string>&);

// Clipboard
std::string readClipboardText();
void setClipboardText(const std::string&);
\`\`\`

### Effects on a Frame

\`\`\`cpp
void setPostEffect(PostEffect*);     // shader pass applied to this frame
void removePostEffect();
void setBackdropEffect(PostEffect*); // effect applied to what's behind
void setBlurRadius(float);           // shorthand: creates internal BlurPostEffect
void setAlphaTransparency(float);
void setCached(bool);                // cache rendered output between frames
void setMasked(bool);
\`\`\`

### Theme glue

\`\`\`cpp
void setPalette(Palette*);
Palette* palette() const;
void setPaletteOverride(theme::OverrideId, bool recursive = true);
float paletteValue(theme::ValueId);
Brush paletteColor(theme::ColorId);
\`\`\`

---

## 4. Drawing — `visage::Canvas`

Header: `visage_graphics/canvas.h`. Inside `draw(Canvas&)`, coordinates are local to the frame. The canvas is **non-copyable** (`Canvas(const Canvas&) = delete`) — don't try to stash one per-frame.

### Color / brush state

\`\`\`cpp
void setColor(unsigned int rgba_word);     // 0xAARRGGBB
void setColor(const Color&);
void setColor(theme::ColorId);
void setBrush(const Brush&);
void setColor(const Brush&);               // alias
void setBlendMode(BlendMode);              // Opaque | Composite | Alpha | Add | Sub | Mult | MaskAdd | MaskRemove
\`\`\`

`BlendMode` is defined in `visage_graphics/graphics_utils.h`.

### Shape primitives

All accept ints, floats, or `Dimension`. All paint in current color/brush.

\`\`\`cpp
void fill();                                              // entire clip region
void fill(x, y, w, h);

void rectangle(x, y, w, h);
void rectangleBorder(x, y, w, h, thickness);

void roundedRectangle(x, y, w, h, rounding);
void roundedRectangleBorder(x, y, w, h, rounding, thickness);
void roundedRectangleShadow(x, y, w, h, rounding, shadow_width);

void leftRoundedRectangle (x, y, w, h, rounding);
void rightRoundedRectangle(x, y, w, h, rounding);
void topRoundedRectangle  (x, y, w, h, rounding);
void bottomRoundedRectangle(x, y, w, h, rounding);

void circle(x, y, diameter);
void ring  (x, y, diameter, thickness);
void fadeCircle(x, y, diameter, pixel_width);
void squircle (x, y, w, power = 4.0f);
void superEllipse(x, y, w, h, power);

void diamond(x, y, w, rounding);
void triangle(ax, ay, bx, by, cx, cy);
void triangleLeft (x, y, width);
void triangleRight(x, y, width);
void triangleUp   (x, y, width);
void triangleDown (x, y, width);

void roundedArc(x, y, diameter, thickness, center_radians, radians);
void flatArc   (x, y, diameter, thickness, center_radians, radians);
void arc       (x, y, diameter, thickness, center_radians, radians, bool rounded);

void segment  (ax, ay, bx, by, thickness, bool rounded);
void quadratic(ax, ay, bx, by, cx, cy, thickness);
\`\`\`

### Text / image / SVG / shader / path

\`\`\`cpp
void text(const String& s, const Font& f, Font::Justification j,
          x, y, w, h, Direction dir = Direction::Up);

void svg(const Svg& s, x, y);
void svg(const EmbeddedFile& f, x, y, w, h);
void svg(const unsigned char* data, int size, x, y, w, h);

void image(const Image& i, x, y);
void image(const EmbeddedFile& f, x, y, w, h);

void shader(Shader* s, x, y, w, h);          // render custom shader into a rect

void fill(const Path&);
void fill(const Path&, x, y);
void fill(const Path&, x, y, w, h);
void stroke(const Path&, x, y, w, h, stroke_width,
            Path::Join = Path::Join::Round,
            Path::EndCap = Path::EndCap::Round,
            std::vector<float> dash_array = {},
            float dash_offset = 0.0f,
            float miter_limit = 4.0f);

// Data viz helpers
void graphLine(const GraphData&, x, y, w, h, thickness);
void graphFill(const GraphData&, x, y, w, h);
void heatMap  (const HeatMapData&, x, y, w, h);
\`\`\`

### State / region

\`\`\`cpp
void saveState();
void restoreState();
void setPosition(float x, float y);                // translate origin
void setClampBounds(x, y, w, h);                   // clip
void setPalette(Palette*);
void setPaletteOverride(theme::OverrideId);

double time() const;          // seconds since start
double deltaTime() const;     // seconds since last draw
int frameCount() const;

float value(theme::ValueId) const;
\`\`\`

### Direct window pairing (advanced — bring-your-own-window only)

\`\`\`cpp
void pairToWindow(void* window_handle, int width, int height);
int submit(int submit_pass = 0);     // push batches to bgfx
void setWindowless(int w, int h);
void removeFromWindow();
\`\`\`

---

## 5. Color, Brush, Gradient

Headers: `visage_graphics/color.h`, `visage_graphics/gradient.h`.

\`\`\`cpp
// Color
Color::Color(uint32_t aarrggbb);
Color::Color(float a, float r, float g, float b);
static Color Color::fromAHSV(float a, float h, float s, float v);

// Gradient (multi-stop)
Gradient g(Color1, Color2, ...);
g.addColorStop(Color, float t);
g.setRepeat(bool);
g.setReflect(bool);

// Predefined gradients
Gradient::kViridis;
Gradient::kMagma;

// Brush — what setColor / setBrush actually takes
static Brush Brush::none();
static Brush Brush::solid(const Color&);

static Brush Brush::horizontal(const Color& left, const Color& right);
static Brush Brush::horizontal(const Gradient&);
static Brush Brush::vertical  (const Color& top,  const Color& bottom);
static Brush Brush::vertical  (const Gradient&);

static Brush Brush::linear(Gradient, const Point& from, const Point& to);
static Brush Brush::radial(Gradient, const Point& center,
                           float radius_x, float radius_y /*, options */);

static Brush Brush::interpolate(const Brush& a, const Brush& b, float t);
\`\`\`

---

## 6. Theme system

Header: `visage_graphics/theme.h`. Macros, not template metaprogramming.

\`\`\`cpp
// Declarations (typically at file or namespace scope)
VISAGE_THEME_COLOR(MyAccent, 0xff00ddaa);     // emits visage::theme::ColorId
VISAGE_THEME_VALUE(CornerRounding, 8.0f);     // emits visage::theme::ValueId
VISAGE_THEME_PALETTE_OVERRIDE(DarkMode);      // emits visage::theme::OverrideId

// Use inside a draw
void draw(visage::Canvas& canvas) override {
  canvas.setColor(MyAccent);
  float r = canvas.value(CornerRounding);
  canvas.roundedRectangle(0, 0, width(), height(), r);
}
\`\`\`

A `Palette` (`visage_graphics/palette.h`) overrides defaults at runtime: `palette.setColor(MyAccent, Color(0xff0099ff));`. Attach to a frame subtree with `frame.setPalette(&palette)` — propagates to children.

Theme IDs are allocated at static init via a global counter, so identifier collisions are not caught at compile time — keep names distinctive.

---

## 7. Input events

Headers: `visage_ui/events.h`, `visage_utils/events.h`.

\`\`\`cpp
struct MouseEvent {
  Point position;               // frame-local
  Point window_position;        // window-relative
  MouseButton button_id;
  int button_state;
  int modifiers;
  bool is_down;
  float wheel_delta_x;
  float wheel_delta_y;
  int repeat_click_count;

  bool isLeftButton()   const;
  bool isMiddleButton() const;
  bool isRightButton()  const;
  bool isAltDown()      const;
  bool isShiftDown()    const;
  bool isCtrlDown()     const;
  bool isCmdDown()      const;
  bool isMainModifier() const;     // Cmd on macOS, Ctrl elsewhere
  bool shouldTriggerPopup() const; // right-click / ctrl-click on macOS
  bool isTouch() const;
};

class KeyEvent {
  KeyCode key_code;
  int modifiers;
  bool key_down;
  bool is_repeat;
  // same modifier predicates as MouseEvent
};

enum MouseButton {
  kMouseButtonNone, kMouseButtonLeft, kMouseButtonMiddle,
  kMouseButtonRight, kMouseButtonTouch
};

enum class MouseCursor {
  Invisible, Arrow, IBeam, Crosshair, Pointing, Dragging,
  HorizontalResize, VerticalResize, TopLeftResize, /* ... */
};

enum class KeyCode { A, B, /* ... */ Z, Number0, /* ... */ Number9,
                     Return, Escape, Backspace, Tab, Space,
                     Up, Down, Left, Right, F1, F2, /* ... */ };
\`\`\`

### Timer + thread marshalling

\`\`\`cpp
class EventTimer {
  void startTimer(int ms);
  void stopTimer();
  virtual void timerCallback();
  std::function<void()>& onTimerCallback();
};

// In visage_ui/events.h:
static inline void runOnEventThread(std::function<void()> fn);   // posts to UI thread
\`\`\`

`runOnEventThread` allocates and locks — it is **not** safe to call from an audio callback. For audio→UI traffic, write to `std::atomic` from the audio thread and poll from a `visage::EventTimer` on the UI thread.

---

## 8. Layout (flex)

Header: `visage_ui/layout.h`. Reached via `frame.layout()`.

\`\`\`cpp
void setFlex(bool);
void setFlexRows(bool);                  // rows vs columns (column = default)
void setFlexReverseDirection(bool);
void setFlexWrap(bool);
void setFlexWrapReverse(bool);
void setFlexGap(Dimension);

void setFlexGrow(float);
void setFlexShrink(float);

void setFlexItemAlignment(ItemAlignment);       // applied to children
void setFlexSelfAlignment(ItemAlignment);       // applied to self
void setFlexWrapAlignment(WrapAlignment);       // line packing when wrapped

enum class ItemAlignment { NotSet, Stretch, Start, Center, End };
enum class WrapAlignment { Start, Center, End, Stretch,
                           SpaceBetween, SpaceAround, SpaceEvenly };

void setMargin(Dimension);
void setMarginLeft(Dimension);
void setMarginRight(Dimension);
void setMarginTop(Dimension);
void setMarginBottom(Dimension);

void setPadding(Dimension);
void setPaddingLeft(Dimension);
void setPaddingRight(Dimension);
void setPaddingTop(Dimension);
void setPaddingBottom(Dimension);

void setWidth(Dimension);
void setHeight(Dimension);
void setDimensions(Dimension w, Dimension h);
\`\`\`

### `Dimension`

Header: `visage_utils/dimension.h`. UDLs are in `namespace visage::dimension`.

\`\`\`cpp
using namespace visage::dimension;

100_px      // logical pixels (DPI-aware)
100_npx     // native pixels (device pixels)
50_vw       // 50% of viewport width
50_vh       // 50% of viewport height
50_vmin     // 50% of min(viewport w, h)
50_vmax     // 50% of max(viewport w, h)
\`\`\`

---

## 9. Animation — `visage::Animation<T>`

Header: `visage_graphics/animation.h`. **A value-smoother, not a tween scheduler.**

\`\`\`cpp
template <typename T>
class Animation {
public:
  Animation(int duration_ms,
            EasingFunction forward  = kLinear,
            EasingFunction backward = kLinear);

  void setSourceValue(T);
  void setTargetValue(T);
  void target(bool targeting, bool jump = false);     // flip toward source/target
  T update();             // call once per frame; returns current eased value
  T value() const;
  bool isAnimating() const;
  bool isTargeting() const;

  void setAnimationTime(int ms);
};

// Time constants
constexpr int kSlowTime    = 240;
constexpr int kRegularTime = 80;
constexpr int kFastTime    = 50;

// Easing functions
EasingFunction kLinear, kEaseIn, kEaseOut, kEaseInOut;
\`\`\`

Drive it from `draw()`: call `update()` once, paint with the result, and re-issue `redraw()` while `isAnimating()` is true so the next frame runs.

---

## 10. Path

Header: `visage_graphics/path.h`. Build vector paths and pass to `canvas.fill()` or `canvas.stroke()`.

\`\`\`cpp
class Path {
public:
  void moveTo(Point);
  void lineTo(Point);
  void quadraticTo(Point control, Point end);
  void cubicTo(Point c1, Point c2, Point end);
  void arc(/* arguments */);
  void close();

  Bounds boundingBox() const;
  float length() const;

  // Compute an offset / stroked path (separate from drawing it)
  Path stroke(float width,
              Join     = Join::Round,
              EndCap   = EndCap::Round,
              std::vector<float> dash_array = {},
              float dash_offset = 0.0f,
              float miter_limit = 4.0f) const;

  enum class Join   { Round, Bevel, Miter };
  enum class EndCap { Butt, Round, Square };
};
\`\`\`

---

## 11. Shaders and post-effects

Headers: `visage_graphics/shader.h`, `visage_graphics/post_effects.h`.

Author shaders in bgfx's `.sc` flavor; CMake compiles them to embeddable bytecode using a bundled `shaderc` binary (see `references/build-and-cmake.md`). The compiled binary is embedded as an `EmbeddedFile` and constructed into a `Shader`.

\`\`\`cpp
class Shader {
public:
  Shader(const EmbeddedFile& vertex_shader,
         const EmbeddedFile& fragment_shader,
         BlendMode);
  // Uniform setters and state accessors
};

class PostEffect { /* base */ };

class BlurPostEffect : public DownsamplePostEffect {
public:
  void setBlurRadius(float);
};

class BloomPostEffect : public DownsamplePostEffect {
public:
  void setBloomSize(float);
  void setBloomIntensity(float);
};

class ShaderPostEffect : public PostEffect {
public:
  ShaderPostEffect(const EmbeddedFile& vs, const EmbeddedFile& fs);
  void setUniformValue(const char* name, /* float[1..4] */);
  void setBlendMode(BlendMode);
};
\`\`\`

Attach: `frame.setPostEffect(&effect);` or `frame.setBackdropEffect(&effect);`.

Examples in the repo: `examples/PostEffects/`, `examples/LiveShaderEditing/`, `examples/shaders/`.

---

## 12. Widgets (optional)

Built only when `VISAGE_ENABLE_WIDGETS=ON` (default ON). Headers live in `visage_widgets/`.

- `Button` (base)
- `UiButton(text)` / `UiButton(text, Font)` — `setText`, `setFont`, `onToggle()` callback `void(Button*, bool)`
- `TextEditor`
- `ScrollBar`, `ScrollableFrame` (in `visage_ui/scroll_bar.h`)
- `PopupMenu` (in `visage_ui/popup_menu.h`)
  \`\`\`cpp
  PopupMenu menu;
  menu.addOption(1, "Option 1");
  menu.addSubMenu("Submenu");
  menu.addBreak();
  menu.onSelection() = [](int id) { /* ... */ };
  menu.show(source_frame, mouse_event.position);
  \`\`\`
- `PaletteColorEditor`, `PaletteValueEditor`, `ShaderEditor`, `ColorPicker`
- `BarList`, `GraphLine`, `HeatMap`, `ShaderQuad`

---

## 13. Resource embedding (CMake helpers)

Defined in `visage_file_embed/CMakeLists.txt`. They compile resources into the binary as `visage::EmbeddedFile { const char* name; const unsigned char* data; int size; }`.

\`\`\`cmake
add_embedded_resources(
  MyTarget
  "my_resources.h"          # generated header you'll include
  "myapp::resources"        # C++ namespace path
  "${CMAKE_CURRENT_LIST_DIR}/assets/icon.svg;${CMAKE_CURRENT_LIST_DIR}/assets/font.ttf"
)

visage_embed_shaders(
  MyTarget
  "my_shaders.h"            # generated header
  "myapp::shaders"          # C++ namespace path
  "${CMAKE_CURRENT_LIST_DIR}/shaders/fs_warp.sc;${CMAKE_CURRENT_LIST_DIR}/shaders/vs_overlay.sc"
)
\`\`\`

Use:

\`\`\`cpp
#include "my_resources.h"
visage::Font font(16.0f, myapp::resources::font_ttf);
\`\`\`

---

## Headers — quick reference

\`\`\`
visage_app/
  application_window.h     — ApplicationWindow
  application_editor.h     — ApplicationEditor
visage_ui/
  frame.h                  — Frame
  events.h                 — MouseEvent, KeyEvent, EventTimer, runOnEventThread
  layout.h                 — Layout
  popup_menu.h             — PopupMenu
  scroll_bar.h             — ScrollBar, ScrollableFrame
visage_graphics/
  canvas.h                 — Canvas
  color.h, gradient.h      — Color, Gradient, Brush
  theme.h, palette.h       — theme macros, Palette
  animation.h              — Animation<T>
  path.h                   — Path
  font.h, image.h          — Font, Image
  shader.h                 — Shader
  post_effects.h           — BlurPostEffect, BloomPostEffect, ShaderPostEffect
  renderer.h               — Renderer singleton
  graphics_utils.h         — BlendMode, Bounds, Point
visage_windowing/
  windowing.h              — Window, createWindow, createPluginWindow
  macos/, win32/, linux/   — platform backends
visage_widgets/
  button.h                 — Button, UiButton
  (and other widgets)
visage_utils/
  dimension.h              — Dimension + UDLs
  events.h                 — KeyCode, MouseButton, MouseCursor
visage_file_embed/
  embedded_file.h          — EmbeddedFile struct
\`\`\`
```

---

===== FILE: references/plugin-integration.md =====

```markdown
# Embedding Visage in an audio plugin

Visage has no built-in plugin framework. The author brings a CLAP/VST3/AU/iPlug2/JUCE wrapper and embeds a `visage::ApplicationWindow` inside the host's editor window. This document explains the contract and the cross-platform gotchas.

## The integration surface (everything you need from Visage)

Plugin integration touches a small slice of the API. Memorize this and most of the work is mechanical.

\`\`\`cpp
// Create
auto app = std::make_unique<visage::ApplicationWindow>();
app->setWindowDimensions(800_px, 600_px);   // initial logical size

// Embed (called when host gives you a parent native handle)
app->show(parent_handle);                    // parent is HWND / NSView* / X11 Window
// or: app->show(w, h, parent_handle);

// React to host-driven resize
app->setNativeWindowDimensions(w_native, h_native);   // Win/Linux
app->setWindowDimensions(w_logical, h_logical);       // macOS (logical points)

// Tell host the editor wants a new size
app->onWindowContentsResized() = [this] {
  host_.requestResize(app->nativeWidth(), app->nativeHeight());
};

// Aspect-ratio / minimum-size hints for the host
app->setFixedAspectRatio(true);
app->setMinimumDimensions(640.0f, 480.0f);

// On Linux, register the X11 fd with the host's POSIX fd support
int fd = app->window()->posixFd();
host_.registerPosixFd(fd, READ | WRITE | ERROR);
// In the host's callback:
app->window()->processPluginFdEvents();

// Tear down
app->close();
app.reset();
\`\`\`

The whole embedding API is on `ApplicationWindow` (`visage_app/application_window.h`) and the underlying `Window` (`visage_windowing/windowing.h`). Plugin frameworks differ in which lifecycle hooks fire these calls, but the Visage calls themselves are the same everywhere.

## The macOS / Windows / Linux pixel-space split

This is the #1 cross-platform bug source. Plugin code that ignores it mis-sizes on Retina and HiDPI displays.

| Platform | Host expects sizes in | Visage method to use |
|---|---|---|
| Windows | Native (device) pixels | `nativeWidth()` / `nativeHeight()` / `setNativeWindowDimensions(int w, int h)` |
| Linux | Native (device) pixels | same as Windows |
| macOS | Logical points (1pt = 1px on non-Retina, 1pt = 2px on Retina) | `width()` / `height()` / `setWindowDimensions(Dimension w, Dimension h)` |

The CLAP example in the repo (`examples/ClapPlugin/clap_plugin.h:50-78`) switches on `__APPLE__` for exactly this. Mirror that pattern:

\`\`\`cpp
#if __APPLE__
  uint32_t pluginWidth()  const { return static_cast<uint32_t>(app_->width());  }
  uint32_t pluginHeight() const { return static_cast<uint32_t>(app_->height()); }
  void setPluginDimensions(uint32_t w, uint32_t h) {
    using namespace visage::dimension;
    app_->setWindowDimensions(Dimension(w), Dimension(h));
  }
#else
  uint32_t pluginWidth()  const { return app_->nativeWidth();  }
  uint32_t pluginHeight() const { return app_->nativeHeight(); }
  void setPluginDimensions(uint32_t w, uint32_t h) {
    app_->setNativeWindowDimensions(int(w), int(h));
  }
#endif
\`\`\`

Wrap host size queries/setters through helpers like these so the plugin layer doesn't think about platforms.

## The parent-handle types per platform

`show(void* parent)` and `createPluginWindow(w, h, void*)` take a single `void*` that means different things:

- **Windows:** `HWND`
- **macOS:** `NSView*` (Visage bridges with `(__bridge NSView*)parent_handle` internally — see `visage_windowing/macos/windowing_macos.mm:875`)
- **Linux/X11:** an `::Window` (X11 window id, which is unsigned long); pass it cast to `void*`

Host frameworks deliver these in framework-specific containers — `clap_window::ptr`, `IPlugView::attached(parent, type)`, VST3's `IPlugFrame`. Always confirm the host passes a real parent of the right type before calling `show(parent)`; some hosts pass a window handle that's not yet realized.

## Linux X11 event-loop integration

Embedded plugins on Linux do **not** get to call `runEventLoop()` — the host owns the loop. The host must drive Visage's X11 file descriptor:

1. Call `int fd = app_->window()->posixFd();` after `show(parent)`.
2. Register `fd` with the host (CLAP's `posix_fd_support`, LV2's `LV2UI_Idle_Interface`, etc.) for `READ | WRITE | ERROR`.
3. When the host fires the fd callback, call `app_->window()->processPluginFdEvents();`.
4. On teardown, deregister the fd before destroying `app_`.

From `examples/ClapPlugin/clap_plugin.cpp:33-39`:

\`\`\`cpp
#if __linux__
  if (_host.canUsePosixFdSupport() && app_->window()) {
    int flags = CLAP_POSIX_FD_READ | CLAP_POSIX_FD_WRITE | CLAP_POSIX_FD_ERROR;
    return _host.posixFdSupportRegister(app_->window()->posixFd(), flags);
  }
#endif
\`\`\`

Wayland is **not supported** by Visage (`BGFX_WITH_WAYLAND OFF`). Linux means X11 only.

## Resize negotiation

Hosts and Visage editors negotiate size both directions:

**Host → editor (host commands a new size)**
1. Host's `guiSetSize(w, h)` (or equivalent) fires.
2. Plugin computes the right `setNativeWindowDimensions` / `setWindowDimensions` per the macOS/Win/Linux table above.
3. Visage relays out children via the flex layout and the editor repaints.

**Editor → host (editor wants a new size)**
1. Something inside the editor changes its content size — e.g., a layout fires, the user grabs a resize handle on a custom corner.
2. `ApplicationWindow` fires its `onWindowContentsResized()` callback.
3. The callback asks the host for permission/cooperation: `_host.guiRequestResize(app_->nativeWidth(), app_->nativeHeight())` (CLAP) or the equivalent in other frameworks.
4. The host responds with `guiSetSize`, which loops back into the host→editor path.

**Aspect-ratio and minimum-size hints**
- `app_->setFixedAspectRatio(true)` and `app_->aspectRatio()` (from `ApplicationEditor`) implement fixed-ratio resizing.
- `app_->setMinimumDimensions(w, h)` enforces a floor.
- `app_->adjustWindowDimensions(uint32_t* w, uint32_t* h, bool h_resize, bool v_resize)` is the function CLAP's `guiAdjustSize` / `guiGetResizeHints` should delegate to — it snaps a host-proposed size to whatever Visage's constraints allow.

## Threading caveats

This matters more for plugins than apps because the audio callback is hard-real-time and runs on a separate thread from the UI.

**bgfx rendering thread.** Off by default (`VISAGE_ENABLE_BACKGROUND_GRAPHICS_THREAD=OFF`). With this off, rendering happens on the thread that owns the host's message loop. Flipping it ON has **not been tested in a plugin context** by upstream — the host may own the message loop in a way that conflicts. Don't change this in a shipping plugin without measuring under multiple hosts.

**`runOnEventThread` is NOT realtime-safe.** `visage::runOnEventThread(std::function<void()>)` lives in `visage_ui/events.h` and is the official way to marshal work from a non-UI thread. But it allocates (a `std::function`) and locks (a queue mutex). Don't call it from an audio callback.

**Canonical audio→UI bridge.** Visage doesn't ship one. The conventional pattern:

\`\`\`cpp
// In your processor — written from audio thread
std::atomic<float> level_meter_db_{ -INFINITY };

// In your editor — owned by the UI
class MeterTimer : public visage::EventTimer {
public:
  MeterTimer(Editor& e) : editor_(e) {}
  void timerCallback() override {
    editor_.updateMeters();      // reads atomics, sets frame state, redraws
  }
  Editor& editor_;
};
MeterTimer timer_{ *this };
// In editor ctor:
timer_.startTimer(33);            // ~30 Hz; pick a rate appropriate to the meter
\`\`\`

`EventTimer` runs on the UI thread, so it can mutate frame state and call `redraw()` directly. Audio callback only writes atomics.

For parameter automation in the other direction (UI knob → audio), use whatever your plugin framework provides (CLAP's parameter events, VST3's `IEditController`, JUCE's `AudioProcessorValueTreeState`). Don't try to call audio-side code directly from a Visage event handler.

## CLAP example — full lifecycle

\`\`\`cpp
class ClapPlugin : public clap_helpers::Plugin {
  // ...

  bool implementsGui() const noexcept override { return true; }

  bool guiIsApiSupported(const char* api, bool is_floating) noexcept override {
    if (is_floating) return false;        // Visage example: embedded only
    #if WIN32
      return std::strcmp(api, CLAP_WINDOW_API_WIN32) == 0;
    #elif __APPLE__
      return std::strcmp(api, CLAP_WINDOW_API_COCOA) == 0;
    #else
      return std::strcmp(api, CLAP_WINDOW_API_X11) == 0;
    #endif
  }

  bool guiCreate(const char* /*api*/, bool /*is_floating*/) noexcept override {
    using namespace visage::dimension;
    app_ = std::make_unique<visage::ApplicationWindow>();
    app_->setWindowDimensions(800_px, 600_px);

    app_->onDraw() = [this](visage::Canvas& c) {
      c.setColor(0xff111122);
      c.fill(0, 0, app_->width(), app_->height());
      // ... real UI here
    };

    app_->onWindowContentsResized() = [this] {
      _host.guiRequestResize(pluginWidth(), pluginHeight());
    };
    return true;
  }

  bool guiSetParent(const clap_window* window) noexcept override {
    app_->show(window->ptr);
#if __linux__
    if (_host.canUsePosixFdSupport() && app_->window()) {
      int flags = CLAP_POSIX_FD_READ | CLAP_POSIX_FD_WRITE | CLAP_POSIX_FD_ERROR;
      return _host.posixFdSupportRegister(app_->window()->posixFd(), flags);
    }
#endif
    return true;
  }

#if __linux__
  void onPosixFd(int fd, int /*flags*/) noexcept override {
    if (app_ && app_->window() && app_->window()->posixFd() == fd)
      app_->window()->processPluginFdEvents();
  }
#endif

  bool guiSetSize(uint32_t w, uint32_t h) noexcept override {
    setPluginDimensions(w, h);
    return true;
  }

  bool guiGetSize(uint32_t* w, uint32_t* h) noexcept override {
    *w = pluginWidth();  *h = pluginHeight();  return true;
  }

  bool guiAdjustSize(uint32_t* w, uint32_t* h) noexcept override {
    app_->adjustWindowDimensions(w, h, true, true);
    return true;
  }

  void guiDestroy() noexcept override {
#if __linux__
    if (app_ && app_->window())
      _host.posixFdSupportUnregister(app_->window()->posixFd());
#endif
    app_->close();
    app_.reset();
  }

private:
  std::unique_ptr<visage::ApplicationWindow> app_;
};
\`\`\`

Full source: `examples/ClapPlugin/` in the Visage repo.

## Adapting to other plugin frameworks

The repo only ships the CLAP example. The pattern translates, but you have to verify the signatures and lifecycle yourself per framework — don't assume.

**VST3 (Steinberg SDK)**
- `IPlugView::attached(void* parent, FIDString type)` → `app_->show(parent)`
- `IPlugView::getSize(ViewRect*)` and `onSize(ViewRect*)` → use the macOS/Win/Linux pixel-space switch
- `IPlugView::canResize()` returns true if you don't set `setFixedAspectRatio`
- `IPlugFrame::resizeView(IPlugView*, ViewRect*)` is what you call from `onWindowContentsResized` to ask the host to resize
- Type strings: `kPlatformTypeHWND`, `kPlatformTypeNSView`, `kPlatformTypeX11EmbedWindowID`

**AU (Apple Audio Unit v2/v3)**
- AUv2: implement an `AUCarbonViewBase`-replacement / `AUViewBase` subclass; pass the `NSView*` the host hands you.
- AUv3 / AppExtension: the host gives you an `AUViewController`; create the `ApplicationWindow` inside `viewDidLoad`, then `app_->show(static_cast<void*>(self.view))`.
- AU runs on the macOS main thread; ensure UI work stays there. `dispatch_async(dispatch_get_main_queue(), ...)` is the safe marshal from any non-UI thread.
- The macOS pixel-space rule still applies — use logical-point sizes.

**iPlug2**
- iPlug2 already abstracts the per-host parent-handle plumbing in `IGraphics::AttachGraphics`. You'll likely write a custom `IGraphics` backend that owns a `visage::ApplicationWindow`. There is no upstream example; expect to write the bridge yourself.

**JUCE (using JUCE only as the plugin wrapper, not its GUI)**
- Subclass `juce::AudioProcessorEditor`. In the constructor, create the `ApplicationWindow` and call `app_->show(getWindowHandle())` (returns `void*` — `HWND` / `NSView*` / X11 `Window`).
- JUCE owns the message thread; do not call `runEventLoop()`.
- JUCE's `paint()` is unused — Visage owns the canvas. You may need to override JUCE's child-component layout to give Visage's view the full bounds.
- Resize: forward `resized()` in the editor to `app_->setNativeWindowDimensions(...)` (or the macOS variant).

**DPF (Distrho Plugin Framework)**
- DPF's `UI` class gives you a native parent via `getParentWindowHandle()`. Same pattern as JUCE.
- DPF wraps its own message loop; let it drive Visage by not calling `runEventLoop()`.

**Caveat to repeat to the user:** none of these adapters exist in the Visage repo. The CLAP path is the only first-party reference. Building a VST3 or AU shim is real work and the macOS pixel-space switch is where you'll spend the most debugging time. Be honest about the gap rather than fabricating headers and method names.

## Resource bundling for plugins

Plugin bundles must be self-contained — fonts, SVG icons, and compiled shaders need to ship inside the `.vst3` / `.component` / `.clap` bundle. Use the CMake helpers to embed them directly into the binary:

\`\`\`cmake
add_embedded_resources(
  MyPlugin
  "plugin_resources.h"
  "myplugin::resources"
  "${CMAKE_CURRENT_LIST_DIR}/assets/icon.svg;${CMAKE_CURRENT_LIST_DIR}/fonts/Inter-Regular.ttf"
)

visage_embed_shaders(
  MyPlugin
  "plugin_shaders.h"
  "myplugin::shaders"
  "${CMAKE_CURRENT_LIST_DIR}/shaders/fs_meter.sc;${CMAKE_CURRENT_LIST_DIR}/shaders/vs_overlay.sc"
)

target_link_libraries(MyPlugin PRIVATE visage)
\`\`\`

Then in C++:

\`\`\`cpp
#include "plugin_resources.h"
#include "plugin_shaders.h"

visage::Font font(14.0f, myplugin::resources::Inter_Regular_ttf);
visage::Shader meter_shader(
  myplugin::shaders::vs_overlay,
  myplugin::shaders::fs_meter,
  visage::BlendMode::Composite
);
\`\`\`

This avoids the "where did my font go" headache when DAWs sandbox plugins or users move them around.

## Sanity checks before shipping

- [ ] Plugin opens correctly in at least one host on every target OS.
- [ ] Logical-vs-native pixel handling verified on a Retina/HiDPI display — UI doesn't shrink to a quarter of the window or balloon to 4x.
- [ ] Resizing from the host's drag handle works without flicker or stale layout.
- [ ] Editor-driven resize (`onWindowContentsResized` → host) round-trips correctly.
- [ ] On Linux, the X11 fd is registered with the host *and* deregistered on `guiDestroy`.
- [ ] No `runOnEventThread` (or anything that allocates/locks) is reachable from the audio callback.
- [ ] Closing the editor doesn't crash the host — `app_->close(); app_.reset();` in the right order.
- [ ] Fonts and shaders are embedded; the plugin runs in a sandbox without external files.
- [ ] DPI changes mid-session (drag window between displays on Windows/macOS) handled — `dpiChanged()` virtual on `Frame` fires; lay out and `redraw()`.
```

---

===== FILE: references/build-and-cmake.md =====

```markdown
# Building with Visage

## Public target

Visage exposes a single CMake target: `visage` (a `STATIC` umbrella defined at `CMakeLists.txt:102` in the repo). It transitively links the per-module object libraries:

- `VisageApp`
- `VisageGraphics`
- `VisageUi`
- `VisageWidgets`
- `VisageWindowing`
- `VisageUtils`

Consumers link only `visage`. The umbrella headers `<visage/app.h>`, `<visage/graphics.h>`, `<visage/ui.h>`, `<visage/widgets.h>`, `<visage/windowing.h>`, `<visage/utils.h>` are **generated by CMake at configure time** into `${CMAKE_BINARY_DIR}/include/visage/` (see `amalgamate_headers` at `CMakeLists.txt:48`). If `<visage/app.h>` isn't resolving in your IDE, a configure run is missing.

## Minimal `FetchContent` setup

\`\`\`cmake
cmake_minimum_required(VERSION 3.17)
project(MyApp CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  visage
  GIT_REPOSITORY https://github.com/VitalAudio/visage.git
  GIT_TAG        main           # pin a tag/commit for reproducible builds
)
FetchContent_MakeAvailable(visage)

add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE visage)
\`\`\`

## `add_subdirectory` setup (vendored Visage)

\`\`\`cmake
add_subdirectory(third_party/visage)   # the cloned repo lives here
target_link_libraries(MyApp PRIVATE visage)
\`\`\`

When consumed as a subdirectory, `VISAGE_BUILD_EXAMPLES` defaults to **OFF**; when Visage is the top-level project, it defaults to **ON**.

## Key CMake options

Set with `-D<NAME>=ON/OFF` or in your own CMake before `FetchContent_MakeAvailable` / `add_subdirectory`:

| Option | Default | Purpose |
|---|---|---|
| `VISAGE_AMALGAMATED_BUILD` | ON | Compiles each module's sources together for faster builds |
| `VISAGE_ENABLE_WIDGETS` | ON | Builds `visage_widgets` (`UiButton`, `TextEditor`, `PopupMenu`, etc.) |
| `VISAGE_ENABLE_BACKGROUND_GRAPHICS_THREAD` | OFF | Enables `BGFX_CONFIG_MULTITHREADED=1`; lets bgfx render on a worker thread |
| `VISAGE_BUILD_EXAMPLES` | ON if standalone, OFF if subdirectory | Builds `examples/` targets |
| `VISAGE_APPLICATION_NAME` | (empty) | Cache string, propagated as a compile definition |
| `VISAGE_ADDRESS_SANITIZER` | OFF | Adds `-fsanitize=address` to relevant targets |

**Don't enable `VISAGE_ENABLE_BACKGROUND_GRAPHICS_THREAD` in a shipping plugin without measuring.** The upstream default is off and the interaction with host-owned message loops isn't documented.

## Dependencies that show up at configure time

Visage's `FetchContent` pulls **bgfx.cmake** (currently `v1.129.8958-499`, see `visage_graphics/CMakeLists.txt:65`). This means:

- A first configure requires **network access** to GitHub.
- The download adds significant time and disk space on first build.
- Wayland support is explicitly disabled (`BGFX_WITH_WAYLAND OFF`). Linux must use X11.

On Apple, Visage enables `OBJC` and `OBJCXX` languages and defaults `CMAKE_OSX_DEPLOYMENT_TARGET` to `10.15` when it's the top-level project (`cmake/compile_flags.cmake`). Bump it if you need newer SDK features.

## Resource embedding

Bundled CMake helpers compile resources into the binary as `visage::EmbeddedFile { const char* name; const unsigned char* data; int size; }`. Definitions live in `visage_file_embed/CMakeLists.txt`.

### Generic resources (fonts, SVGs, PNGs, etc.)

\`\`\`cmake
add_embedded_resources(
  MyTarget                                # target to attach to
  "my_resources.h"                        # generated header
  "myapp::resources"                      # C++ namespace path
  "${CMAKE_CURRENT_LIST_DIR}/assets/Lato-Regular.ttf;${CMAKE_CURRENT_LIST_DIR}/assets/logo.svg"
)
\`\`\`

\`\`\`cpp
#include "my_resources.h"

visage::Font font(16.0f, myapp::resources::Lato_Regular_ttf);
canvas.svg(myapp::resources::logo_svg, 0, 0, 64, 64);
\`\`\`

The generated identifier replaces dots and hyphens with underscores (e.g., `Lato-Regular.ttf` → `Lato_Regular_ttf`).

### Shaders (bgfx `.sc` source)

\`\`\`cmake
visage_embed_shaders(
  MyTarget
  "my_shaders.h"
  "myapp::shaders"
  "${CMAKE_CURRENT_LIST_DIR}/shaders/fs_warp.sc;${CMAKE_CURRENT_LIST_DIR}/shaders/vs_overlay.sc"
)
\`\`\`

Behind the scenes this runs `shaderc` (bundled in `visage_graphics/bin/{win32,macos,linux}/shaderc`) to compile each `.sc` into platform-appropriate bytecode for D3D11/Metal/Vulkan/GLES, then packs the result as embedded data.

\`\`\`cpp
#include "my_shaders.h"

visage::ShaderPostEffect warp(myapp::shaders::vs_overlay, myapp::shaders::fs_warp);
warp.setUniformValue("intensity", 0.8f);
frame.setPostEffect(&warp);
\`\`\`

Examples of authoring `.sc` shaders: `examples/shaders/` in the Visage repo.

## Multi-platform target setup tips

**macOS** — Visage manages the `OBJC/OBJCXX` enablement. If you set your own `CMAKE_OSX_DEPLOYMENT_TARGET`, do it before Visage's `add_subdirectory` / `FetchContent_MakeAvailable` so Visage picks it up.

**Windows** — No extra work for a standalone .exe. For a plugin (`.vst3` / `.clap`), build a `MODULE` target with the host SDK's CMake helpers and `target_link_libraries(... PRIVATE visage)`.

**Linux** — X11 dev libraries must be installed (`libx11-dev`, `libxext-dev`, `libxcursor-dev`, `libxinerama-dev`, `libxrandr-dev`). Plugin authors also need `libdbus-1-dev` if their host SDK uses it.

**Emscripten** — Visage supports Web target via bgfx's GLES2/WebGL2 backend. The CLAP example is excluded from the Emscripten build (`examples/CMakeLists.txt`).

## Build hygiene

- **Pin the `GIT_TAG`** in `FetchContent_Declare` to a specific commit or release tag in any project you care about being reproducible. `main` is fine for prototyping but will surprise you later.
- **Run a full configure on a clean build directory** after upgrading Visage — generated headers change.
- **Don't `#include` from `visage_graphics/`, `visage_ui/` etc. paths in headers you ship** to other targets — prefer the `<visage/foo.h>` umbrella. The umbrella absorbs additions; the fine-grained headers can move.
- **Keep `VISAGE_AMALGAMATED_BUILD=ON`** unless you're debugging a specific compile error in one source file.
- **If you bundle shaders, run a clean configure after editing a `.sc`** — incremental dependencies can miss shader regenerations in some generators.

## Common configure errors

**`Could not find dependency 'bgfx'`** — Network was unreachable during `FetchContent`. Delete the `_deps/` directory under your build folder and re-configure with network access.

**`<visage/app.h> not found`** — You haven't configured (the header is generated). Run `cmake -S . -B build` once.

**`'OBJC' language not enabled`** (Apple) — Something else in the CMake graph called `project(... LANGUAGES CXX)` and bumped Visage's `enable_language(OBJC)` out of effect. Add `enable_language(OBJC OBJCXX)` near the top of your own CMake.

**`cannot find -lX11`** (Linux) — Install the X11 dev packages listed above.

**`shaderc: command not found`** during `visage_embed_shaders` — The bundled binary in `visage_graphics/bin/<platform>/` isn't executable. `chmod +x` it (most often a checkout issue on Linux/macOS).
```

---

## Sources

- Visage repository: https://github.com/VitalAudio/visage
- CLAP integration reference: `examples/ClapPlugin/` in the Visage repo
- API signatures quoted from headers under `visage_app/`, `visage_ui/`, `visage_graphics/`, `visage_windowing/`, `visage_widgets/`, `visage_utils/`, `visage_file_embed/`
