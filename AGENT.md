# AGENT.md — TestingPlaygroundSynth

Reference for AI agents working in this repository. Read this before
touching any file. It covers what every file does, the patterns you must
follow when extending the plugin, the threading contract that must never
be broken, and the build mechanics that are easy to get wrong.

---

## 1. What this project is

A JUCE audio plugin template that compiles as:

- **AU** (Audio Unit) — installed to `~/Library/Audio/Plug-Ins/Components/`
- **VST3** — installed to `~/Library/Audio/Plug-Ins/VST3/`
- **Standalone** — macOS app; runs from the build tree

The GUI uses [visage](https://github.com/VitalAudio/visage) (VitalAudio's
GPU-accelerated graphics library) instead of JUCE's built-in drawing.
visage renders via Metal on macOS. All knob drawing is done with custom
bgfx fragment shaders.

**Platform constraint:** macOS 14+, Apple Silicon (arm64) only. The
CMakeLists.txt will hard-error on any other platform.

---

## 2. Repository layout

```
CMakeLists.txt              Build config; all rename points are marked
scripts/
  plugin.py                 clone / rename / codesign / notarize
Source/
  PluginProcessor.h/.cpp    AudioProcessor + APVTS + parameter dispatch
  PluginEditor.h/.cpp       AudioProcessorEditor + visage host
  StandaloneApp.cpp         Custom standalone window (Standalone target only)
  dsp/
    Gain.h                  Example DSP block — replace with your own
  gui/
    Knob.h/.cpp             Rotary knob widget (visage + bgfx shader)
shaders/
  vs_shader_quad.sc         Vertex shader (bgfx/shaderc source)
  fs_knob_background2.sc    Fragment shader for knob appearance
  varying.def.sc            bgfx varying declarations
fonts/
  Roboto_Condensed-Regular.ttf  Embedded font for knob value text
build/                      CMake output (excluded from clone; created fresh)
```

---

## 3. Source file guide

### `CMakeLists.txt`

Every rename point when forking is in the comment block at the top of the
file. Key sections:

- **Build-type guard** (lines 21–23): Forces `Release` before anything
  else. Do not remove it. Mismatched debug/release between visage's static
  library and the plugin TUs causes an ODR layout drift on `visage::Canvas`
  (extra `LeakChecker` member in debug) that silently corrupts Metal state
  reads and crashes on the first non-trivial shape. See the inline comment
  for details.
- **`FetchContent_Declare`**: JUCE is pinned to `8.0.4`; visage is pinned
  to commit `b0b2ee89`. Bump visage deliberately — its API moves.
- **`visage_embed_shaders` / `add_embedded_resources`**: Compile `.sc`
  files in `shaders/` to Metal bytecode at build time and pack them as C++
  byte arrays in a generated header. Same for fonts. Headers land in
  `build/…_artefacts/Release/EmbeddedShaderResources_generated/` and are
  included as `"embedded/plugin_shaders.h"` and `"embedded/plugin_fonts.h"`.
- **`target_compile_options`**: `-mcpu=apple-m1`, `-fno-math-errno`,
  `-fno-trapping-math`. **Never add `-ffast-math`** — JUCE uses
  `std::numeric_limits<>::infinity()` in `constexpr` contexts, which is UB
  under `-ffinite-math-only` and causes compile errors.
- **`add_custom_command PRE_BUILD` (AU + VST3 targets)**: Runs `xattr -cr`
  on the build-directory bundle before JUCE's own ad-hoc signing step.
  Prevents "resource fork / detritus not allowed" errors on incremental
  rebuilds after a `plugin.py codesign` run has left extended attributes on
  the bundle.

### `Source/PluginProcessor.h`

Declares `TestingPlaygroundSynthProcessor : public juce::AudioProcessor`.

- **`pid` namespace**: string IDs for every APVTS parameter. Add new IDs
  here before anything else.
- **APVTS**: `juce::AudioProcessorValueTreeState apvts_` with a static
  `createLayout()` factory. Every automatable parameter must be registered
  here.
- **DSP ownership**: The processor owns the DSP objects (e.g., `gain_`).
  DSP objects must be cheap to construct and must not allocate on the audio
  thread.
- **`parameterChanged()`**: The APVTS listener callback. Dispatches
  parameter changes from the message thread to DSP setters. Runs on the
  message thread, not the audio thread — do not touch audio buffers here.
- **MIDI / bus flags**: `acceptsMidi()`, `producesMidi()`, `isMidiEffect()`
  are one-liners that return the values set by `plugin.py set_plugin_type`.

### `Source/PluginProcessor.cpp`

Implements the processor.

- **Constructor**: Registers `this` as an APVTS listener; sets initial DSP
  state from the current parameter values.
- **`prepareToPlay(sr, bs)`**: Calls `prepare(sr, bs)` on every DSP object.
  This is the only place block-size-dependent allocation is allowed.
- **`processBlock(buffer, midi)`**: Calls `processBlock()` on every DSP
  object in order. No allocation, no APVTS access, no locks.
- **Bus layout**: `BusesProperties` in the constructor init list and
  `isBusesLayoutSupported()` must agree. `plugin.py` keeps them in sync
  when changing plugin type; if you edit them manually, keep both consistent.

### `Source/PluginEditor.h/.cpp`

Hosts a `VisageHost` JUCE component that wraps `visage::ApplicationWindow`.

- **Deferred initialisation**: visage needs an OS-level window handle,
  which is only available after the JUCE component is shown. The window is
  created in `visibilityChanged()` / `parentHierarchyChanged()` once the
  peer exists. Do not call visage draw APIs before the window is
  initialised.
- **`rootFrame_`**: A `visage::Frame` that covers the entire editor. Add
  child frames or widgets to this frame.
- **`onDraw()` lambda**: Runs every frame on the GPU/render thread. Do
  not call JUCE APIs or touch APVTS from inside `onDraw()`.
- **Knob binding**: `knob_.bind(&param)` connects the widget to an APVTS
  parameter so it reads/writes the normalised value via
  `param->setValueNotifyingHost()`.

### `Source/StandaloneApp.cpp`

Only compiled in the Standalone target
(`JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1`). Replaces JUCE's default
standalone wrapper with a `DocumentWindow` that uses the native macOS title
bar and suppresses the feedback-loop mute banner and options gear button.
You rarely need to modify this file.

### `Source/dsp/Gain.h`

The canonical DSP block template. Every DSP class you write should follow
this shape:

```cpp
class MyDsp {
public:
    void prepare(double sampleRate, int blockSize);
    void setSomeParam(float value);          // called from message thread
    void processBlock(float** channels, int nChannels, int nSamples);
private:
    std::atomic<float> target_{};            // written by message thread
    float current_{};                        // read only by audio thread
};
```

Key rules:
- Cross-thread communication goes through `std::atomic` members.
- The audio thread reads atomics, applies smoothing, and updates local
  state. It never writes back to atomics.
- `processBlock()` does no allocation and holds no locks.
- `prepare()` is the only method that may allocate (e.g., resize a delay
  line). It is called from the message thread before playback starts.

### `Source/gui/Knob.h/.cpp`

A template class `Knob<ParamType>` (typically `AudioParameterFloat`).

- Renders the knob via `fs_knob_background2.sc` by packing three state
  values into a `visage::Color` passed as a uniform:
  - **R channel**: normalised parameter value `[0, 1]`
  - **G channel**: hover-ring thickness `[0, 1]` (animated 40ms in/out)
  - **B channel**: squareness flag (`0x00` or `0xff`)
- Text overlay: `param->getCurrentValueAsText()` in Roboto Condensed,
  scaled to fit inside the knob.
- Mouse: vertical drag adjusts value; Ctrl+drag applies 0.2× sensitivity
  for fine adjustment.
- `bind(AudioParameterFloat*)` connects to the parameter. Only call this
  after the parameter exists in the APVTS.

### `shaders/`

bgfx shader sources (`.sc` files). Do not edit these unless you understand
bgfx's cross-platform shader language and the visage embedding pipeline.
After editing a shader, delete
`build/EmbeddedShaderResources_generated/` and rebuild — CMake will
recompile the shaders and regenerate the header.

---

## 4. Threading model

This is the most important section. Breaking the threading contract causes
data races and undefined behaviour that may not crash immediately.

```
Message thread (JUCE / UI)
  │
  ├─ APVTS listens for DAW automation changes
  ├─ parameterChanged(id, newValue) fires here
  ├─ Calls typed DSP setter, e.g. gain_.setGainDb(v)
  └─ Setter writes to std::atomic<float> target_
                         │
                         │  (atomic load, no lock)
                         ▼
Audio thread (processBlock)
  └─ Reads target_, smooths toward it, applies to samples
```

**Rules that must never be violated:**

1. `processBlock()` does not read from or write to APVTS.
2. `processBlock()` does not allocate heap memory.
3. `processBlock()` does not take any lock (mutex, spin-lock, etc.).
4. DSP setters write only to `std::atomic` members; they do not touch
   audio buffers.
5. visage `onDraw()` lambdas do not call JUCE APIs or touch APVTS.
6. `prepareToPlay()` may allocate; `processBlock()` may not.

---

## 5. How to add a new parameter

Follow these steps in order. The existing `Gain` parameter is the
reference implementation for each step.

**Step 1 — Declare the parameter ID** in `PluginProcessor.h`:
```cpp
namespace pid {
    inline constexpr const char* gain    = "gain";
    inline constexpr const char* myParam = "myParam";  // add here
}
```

**Step 2 — Register in `createLayout()`** in `PluginProcessor.cpp`:
```cpp
layout.add(std::make_unique<juce::AudioParameterFloat>(
    juce::ParameterID{pid::myParam, 1},
    "My Param",
    juce::NormalisableRange<float>{0.0f, 1.0f},
    0.5f));
```

**Step 3 — Add a DSP setter** and call it from `parameterChanged()`:
```cpp
void TestingPlaygroundSynthProcessor::parameterChanged(
    const juce::String& id, float v)
{
    if (id == pid::gain)    gain_.setGainDb(v);
    if (id == pid::myParam) myDsp_.setMyParam(v);  // add here
}
```

Also set the initial value in the constructor, after adding the listener:
```cpp
myDsp_.setMyParam(apvts_.getRawParameterValue(pid::myParam)->load());
```

**Step 4 — Bind a GUI widget** in `PluginEditor.cpp`:
```cpp
auto* p = dynamic_cast<juce::AudioParameterFloat*>(
    processor_.apvts_.getParameter(pid::myParam));
myKnob_.bind(p);
```

---

## 6. How to add a new DSP file

1. Create `Source/dsp/MyDsp.h` following the `Gain.h` pattern. If it
   needs a `.cpp`, create `Source/dsp/MyDsp.cpp` as well.
2. Add the `.cpp` to `CMakeLists.txt`:
   ```cmake
   target_sources(TestingPlaygroundSynth PRIVATE
       Source/PluginProcessor.cpp
       Source/PluginEditor.cpp
       Source/StandaloneApp.cpp
       Source/gui/Knob.cpp
       Source/dsp/MyDsp.cpp)   # add here
   ```
3. `#include "dsp/MyDsp.h"` in `PluginProcessor.h` and add a member.

---

## 7. How to add a new GUI widget

1. Create `Source/gui/MyWidget.h/.cpp` as a `visage::Frame` subclass
   (or use the `Knob` template for rotary controls).
2. Add the `.cpp` to `target_sources` in `CMakeLists.txt`.
3. Add a member to `TestingPlaygroundSynthEditor` in `PluginEditor.h`.
4. In `PluginEditor.cpp`, add the widget as a child of `rootFrame_` inside
   the visage initialisation block, and position it.
5. If the widget needs a custom shader, add the `.sc` files to `shaders/`
   and re-run the build to trigger shader compilation.

---

## 8. Build system

```sh
# Configure (first time or after CMakeLists.txt changes):
cmake -B build -G Ninja

# Build:
cmake --build build --config Release

# Full clean rebuild:
rm -rf build && cmake -B build -G Ninja && cmake --build build --config Release
```

`COPY_PLUGIN_AFTER_BUILD TRUE` in CMakeLists.txt means a successful build
automatically installs the AU to `~/Library/Audio/Plug-Ins/Components/` and
the VST3 to `~/Library/Audio/Plug-Ins/VST3/`. The Standalone binary is only
in `build/TestingPlaygroundSynth_artefacts/Release/Standalone/`.

**After editing a shader**, delete
`build/TestingPlaygroundSynth_artefacts/Release/EmbeddedShaderResources_generated/`
before rebuilding so CMake re-runs shaderc.

**After editing `CMakeLists.txt`** (not just a rebuild), re-run the
configure step (`cmake -B build -G Ninja`) before building.

---

## 9. scripts/plugin.py

Subcommands — all run from the project root:

```sh
./scripts/plugin.py clone       # interactive: clone into a new plugin
./scripts/plugin.py rename      # interactive: rename current plugin in place
./scripts/plugin.py codesign --identity "Developer ID Application: Name (ID)"
./scripts/plugin.py notarize --keychain-profile "PROFILE_NAME"
```

**`clone`**: Prompts for name, type, company, codes, bundle ID, DSP
namespace, destination. Copies the source tree (excluding `build/`, `.git`,
`.DS_Store`, etc.), creates a fresh empty `build/` directory in the clone,
then performs case-sensitive bulk string replacement throughout all text
files. Finally calls `set_plugin_type()` to configure bus layout, MIDI
flags, and `IS_SYNTH`.

Auto-discovers the current plugin's metadata from `CMakeLists.txt` and
`Source/dsp/*.h` (for the DSP namespace), so it works from any plugin
forked from this template — not just the original.

**`rename`**: Same replacement logic as clone, applied in place. Wipe
`build/` after renaming and re-run cmake.

**`codesign`**: Runs `xattr -cr` on each bundle first to strip extended
attributes, then calls `codesign --force --deep --options runtime --timestamp`.
Required before notarization.

**`notarize`**: Zips each bundle with `ditto`, submits via
`xcrun notarytool submit --wait`, then staples the ticket with
`xcrun stapler staple`. Requires a keychain profile set up via
`xcrun notarytool store-credentials`.

---

## 10. Known gotchas

| Situation | What goes wrong | Fix |
|-----------|----------------|-----|
| Build type not forced to Release | visage `LeakChecker` member present in debug builds causes ODR layout drift → crash inside Metal state reads | The `if(NOT CMAKE_BUILD_TYPE…)` guard at the top of CMakeLists.txt must remain |
| `-ffast-math` added to compile flags | JUCE `constexpr` uses of `std::numeric_limits<>::infinity()` become UB → compile errors | Never add `-ffast-math`; use `-fno-math-errno -fno-trapping-math` instead |
| Shader source edited without clean | Stale generated header used → old shader runs | Delete `build/…/EmbeddedShaderResources_generated/` and rebuild |
| APVTS accessed from `processBlock()` | Data race on parameter value tree | All APVTS reads must happen on the message thread; use atomics to pass values to the audio thread |
| visage draw APIs called before window is ready | Null dereference inside Metal layer setup | visage init is guarded by `visibilityChanged()` / `parentHierarchyChanged()`; do not call draw APIs before that fires |
| New `.cpp` file not added to `target_sources` | File silently ignored by the build | Always add new `.cpp` files to `target_sources(TestingPlaygroundSynth PRIVATE …)` |
| Plugin renamed manually without wiping build/ | Stale object files with old symbols cause linker errors | After any rename run `rm -rf build/` and reconfigure |
| Rebuilt after `plugin.py codesign` | JUCE's copy step fails: "file INSTALL cannot set permissions — Operation not permitted" because codesign set immutable flags on the installed bundle | `chflags -R nouchg ~/Library/Audio/Plug-Ins/…/<name>.component` (and `.vst3`), or delete both installed bundles and rebuild |

---

## 11. Allowed shell operations

The `.claude/settings.local.json` allowlist permits:

- `cmake` (configure and build invocations)
- `ninja`, `make` (build runners)
- `git`, `otool`, `objdump`, `nm` (inspection)
- `bash` (general shell)
- Copying shaders and fonts from sibling plugin directories

Do not attempt network access, keychain access, or system-level changes
outside this list.
