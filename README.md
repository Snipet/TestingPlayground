# TestingPlaygroundSynth

An audio plugin to start from: AU, VST3 and Standalone for macOS on Apple silicon, built on JUCE 8.0.4, with a GPU
editor made with [FunkGui](https://github.com/Snipet/FunkGui) and the DSP core of FSynth (SIMD, fast math, phase
accumulators, smoothers, random numbers), vendored. One command makes a new plugin from it, an effect or an
instrument.

<!-- starter:intro -->
As it ships it is an instrument: eight voices, each a sine or a triangle under an attack/release envelope, played
by MIDI, stereo out.
<!-- /starter:intro -->

The editor is drawn from the parameter table: a dial or a slider per parameter, the theme and zoom cells. After a
build, `build-dev/probe-out/gui.dump.png` is a picture of it (the `gui.dump` probe renders it headless).

## Build and run

Requirements: macOS 14 or later on Apple silicon, Xcode or its command-line tools, CMake 3.30 or later, Ninja,
Python 3. JUCE, bgfx and the shader compiler come from the machine cache `~/audio/.deps` (FCompressor's
`Scripts/deps.sh` fills it), FunkGui from `~/audio/libraries/FunkGui` at the pinned tag (`cmake/Deps.cmake`); a
machine without them fetches them from GitHub instead.

```sh
cmake --workflow --preset dev-verify     # configure, build everything, run every test (build-dev/)
open build-dev/TestingPlaygroundSynth_artefacts/RelWithDebInfo/Standalone/TestingPlaygroundSynth.app
```

The Standalone of an effect starts with its input muted (JUCE's guard against feedback): unmute it in its Options
(the cog in the title bar), with headphones on. For a host:

```sh
cmake --workflow --preset release-build  # Release with LTO (build-release/)
scripts/install.sh                       # copy the AU and VST3 into ~/Library/Audio/Plug-Ins
```

Builds never install anything by themselves; `scripts/install.sh build-dev` installs a dev build instead.

## What is where

```
cmake/Product.cmake      the identity: name, kind (effect or instrument), version, codes, bundle id, C++ namespace
Source/dsp/              the algorithm, plain C++20 without JUCE: Params.h (the parameter table), Engine.h/.cpp
Source/fsdsp/core/       FSynth's DSP core, vendored at the tag in SEED.tsv (scripts/sync-dsp-core.sh)
Source/plugin/           the JUCE AudioProcessor: parameters from the table, the Engine called once per block
Source/editor/           the FunkGui panel, built from the table; Theme.h (colours); gpu/ starts it in the plugin
Tools/probes/            tests that measure: dsp/ (the algorithm alone), plugin/, gui/ (the panel, headless)
scripts/                 plugin.py, test.sh, sync-dsp-core.sh, install.sh; starters/ (effect and instrument code)
CMakeLists.txt, cmake/   the build; every source list is a folder glob, so new files need no CMake edit
```

## Change the algorithm

`Source/dsp/Engine.cpp` is the whole algorithm: `prepare()` before playback, `setParams()` once per block with the
plain parameter values, `process()` once per block (an effect processes the channels in place; an instrument writes
them, playing the block's note events at their offsets). Rewrite its body, or add your own classes under
`Source/dsp/` and call them from it. The rules of the audio thread: no allocation, locks or I/O in `setParams()` and
`process()` (allocate in `prepare()`), and smooth every parameter so nothing steps. The core helps: `fsdsp::simd`
(four float lanes, NEON), `fsdsp::exp2/log2/tanh/sinPi/tanPi` (no libm), `fsdsp::phase` (exact oscillators),
`fsdsp::Smoother4`, `SmoothRamp`, `Pcg32`, `sanitize`. Each header says what it guarantees.

## Add a control

A parameter is one row of the table in `Source/dsp/Params.h`:

```cpp
namespace param { enum Index : std::size_t { gain, /* ... */ mix, count }; }    // 1. an index, before count

{ .id = "mix", .name = "Mix", .label = "MIX", .unit = Unit::percent,             // 2. its row, at the same place
  .min = 0.0f, .max = 100.0f, .def = 100.0f, .centre = 50.0f, .control = Control::dial },
```

then `values[param::mix]` in `Engine::setParams` (3). The host parameter, the saved state, the panel's control and
its text follow from the row: `control` is `dial`, `slider`, `choice` (cells, with `.choices`/`.numChoices`) or
`hidden`; `centre` is the value at the middle of the control's travel (the midpoint for linear, lower to give the low
end more room, as for frequencies). Keep IDs forever: hosts store them in sessions.

For a view of your own (a graph, a meter), add an `Item` in `Source/editor/Panel.cpp` and place it in the
constructor: FunkGui widgets (`Dial`, `RuleSlider`, `SegmentedSelector`, `LatchToggle`, ...) or your own drawing on the
`funkgui::Canvas`. The panel is GPU-free: the plugin runs it on Metal, the `gui.dump` probe on the CPU.

## Test

```sh
scripts/test.sh                  # build the probes, run them; outputs in build-dev/probe-out/
scripts/test.sh -R dsp.response -V
build-dev/TestingPlaygroundSynth_probes --list
```

<!-- starter:probes -->
| Probe | Checks |
| --- | --- |
| `dsp.render` | plays an arpeggio and a chord (sine, then triangle) to `dsp.render.wav`; finite, no clipping, block-size invariant |
| `dsp.response` | pitch within 0.005 Hz; level, GAIN and velocity within 0.02 dB; the attack and release shapes; each of eight held notes in the output; voice stealing; no click when a voice is retriggered or stolen |
| `plugin.process` | the processor as a host drives it: a note-on sounds from its sample offset at its velocity, the note-off releases it to silence with no voice left, GAIN written by the host reaches the DSP, a NaN from the host is ignored, both channels are the Engine's output bit for bit |
| `gui.dump` | the panel headless to `gui.dump.png` and `gui.dump.paper.png`; settles, every glyph, a11y, a drag writes the parameter |
| `plugin.state` | the host's parameters equal the table; the state round-trips; processBlock gives finite output |
<!-- /starter:probes -->

`dsp.*` and `plugin.process` test the algorithm and come with its starter (`scripts/starters/<kind>/`): change
them with it. `gui.dump` and `plugin.state` work from the parameter table, whatever the algorithm.

A probe is one file in `Tools/probes/<layer>/` whose first line registers it (`// PROBE name=dsp.mytest timeout=60`)
and whose body uses spec rows (`P.near`, `P.le`, `P.ge`, `P.in`, `P.eq`): each states what must hold, with its
tolerance. `Tools/probes/common/Audio.h` has the helpers (sines, levels, a DFT bin, frequency, WAV output).

## Make a new plugin

```sh
scripts/plugin.py clone --name MyDelay                         # an effect, beside this project (../MyDelay)
scripts/plugin.py clone --name MySynth --type instrument       # an instrument: MIDI in, eight voices, attack/release
scripts/plugin.py clone --name MyDelay --dest ~/dev/MyDelay --code Mydl --yes
```

The clone gets its own name, plugin code, bundle id and C++ namespace (asked for, or derived from the name), keeps
the manufacturer code, and builds without edits. It refuses a plugin code another project beside it already uses: two
plugins with one code hide each other in a host. `--type` switches the kind: the algorithm, its probes and this
README's kind-specific blocks (the ones between `<!-- starter:... -->` markers) become that kind's starter from
`scripts/starters/`. `--starter` restarts from the starter of the same kind; `plugin.py rename` renames this
project in place; `plugin.py starter-diff` shows what you changed from the starter.

## Update the DSP core

```sh
scripts/sync-dsp-core.sh <fsynth-tag>     # e.g. fsyn-s3 (the seed, SEED.tsv) or a later fsyn-sN
```

It takes `Source/fsdsp/core` from FSynth at that tag (`git tag` in FSynth lists them) and records it in SEED.tsv.
It refuses to overwrite a core file you edited (`--force` overwrites). Build and test after a sync.

## Theme, zoom, diagnostics

The THEME cells (header) and the ZOOM cells (footer) are machine-wide preferences, stored by FunkGui in
`~/Library/Application Support/TestingPlaygroundSynth/`. Colours are tokens: `Source/editor/Theme.h` shows how to give
this plugin its own accent. `TSYN_UI_THEME=1` opens the Standalone on the light theme without saving it,
`TSYN_GPU_LOG=1` logs the GPU surface (the prefix is the product's, `cmake/Product.cmake`).
