# TestingPlayground

A playground for researching feedback delay network (FDN) reverbs: an effect (AU, VST3 and Standalone) for macOS on
Apple silicon, built on JUCE 8.0.4, with a GPU editor made with [FunkGui](https://github.com/Snipet/FunkGui) and the
DSP core of FSynth (SIMD, fast math, smoothers), vendored. It was made from FunkPluginTemplate with
`scripts/plugin.py clone`, and its reverb is the FDN of the Visage version (git tag `legacy-visage`), ported and
completed.

After a build, `build-dev/probe-out/gui.dump.png` is a picture of the editor and `dsp.render.ir.wav` beside it is the
reverb's impulse response.

## The reverb

`Source/dsp/Fdn.h` says it all in one comment; in short, per sample, for the n lines (LINES: 4, 8 or 16):

- **Delay lengths from the room** (`FdnLayout.h`): SIZE is a room in metres. Its mean free path, 2 S / 3, sets the
  centre of n lengths spread over one octave; each is snapped to the nearest prime (the legacy `findClosestPrime`
  rule, `Primes.h`) and raised to the next one if it would repeat, so the lengths are distinct primes, pairwise
  coprime. At 12 m and 48 kHz: 787, 829, ... 1583 samples. The editor's DELAYS line shows the current ones.
- **Feedback matrix** (`Hadamard.h`): an orthonormal Hadamard matrix, applied by a fast Walsh-Hadamard transform. It
  keeps the energy that circulates, so the decay is the gains' alone.
- **Decay**: line i's gain is 10^(-3 D_i / (T fs)) with T = DECAY: every line loses 60 dB in DECAY seconds whatever
  its length. **HF DECAY** turns that gain into a one-pole absorption filter with the same gain at 0 Hz and the gain
  of a decay HF DECAY times shorter at Nyquist (100 % = no damping).
- **Delay lines** (`DelayLine.h`): read through a first-order Thiran allpass (exactly unity magnitude, so the
  interpolation never adds or removes level in the loop), as the legacy delay line was.
- **Modulation**: an LFO moves each line's read by up to DEPTH (ms), at RATE (Hz), with the shape you draw in the
  editor's graph (`LfoShape.h`); line i runs i/n of a cycle behind line 0.
- **Input and output**: the left input feeds the even lines and the right the odd ones, with fixed signs, scaled by
  sqrt(1 - mean g^2) so that steady noise comes out near its own level at any SIZE and DECAY; the even lines make
  the left output, the odd lines the right. MIX blends dry and wet; GAIN is the output level. Each input reaches the
  network through a DC blocker (a one-pole high-pass at 5 Hz): 0 Hz is always a mode of the network, and without it
  a DC offset would come out amplified (+12 dB at the defaults, +34 dB at 1 m, 4 lines and DECAY 20 s). NaN and
  infinite input samples become 0, and the input is clamped to +-1e6, before any delay line.
- **Nothing steps**: parameters are smoothed (20 ms); a SIZE change glides the delays to their new primes (a pitch
  slide, 60 ms); a LINES change fades the wet path out, switches and clears the network, and fades back in (20 ms
  each). The output does not depend on how the host splits its blocks.

| Parameter | Range | Default | What |
| --- | --- | --- | --- |
| SIZE | 1 to 100 m | 12 m | the room: the delay lengths scale with it |
| DECAY | 0.1 to 20 s | 2 s | the time to fall 60 dB (at low frequencies) |
| HF DECAY | 10 to 100 % | 50 % | the decay time at the top of the spectrum, as a part of DECAY |
| MIX | 0 to 100 % | 30 % | dry to wet |
| GAIN | -24 to +24 dB | 0 dB | the output (the legacy plugin's one parameter) |
| LINES | 4, 8, 16 | 16 | the number of delay lines, the Hadamard matrix's order |
| RATE | 0.01 to 20 Hz | 0.5 Hz | the modulation LFO's rate |
| DEPTH | 0 to 2 ms | 0.25 ms | the largest change of a line's delay (at most 45 % of the line) |

## The editor

The reverb's dials and the LINES cells across the top; below them the modulation: the LFO shape and the RATE and
DEPTH dials; under the graph the DELAYS line; THEME (header) and ZOOM (footer) cells; the footer's left side reads out
the control under the hand.

The graph is one LFO cycle, +1 (the longest delay) at the top and -1 at the bottom:

- drag a point to move it (Alt snaps to the grid of eighths, Cmd keeps it between its neighbours); the two end points
  are one point of the cycle and move together;
- drag the small handle in the middle of a segment up or down to bend it;
- double-click an empty spot to add a point (hold the button to keep dragging it), double-click a point to remove it;
- right-click for Delete point and Reset shape; with the graph focused (Tab), the arrow keys move the selected point
  and Delete removes it.

The shape is saved with the plugin's state (not as a host parameter: hosts neither automate nor undo it).

## Build and run

Requirements: macOS 14 or later on Apple silicon, Xcode or its command-line tools, CMake 3.30 or later, Ninja,
Python 3. JUCE, bgfx and the shader compiler come from the machine cache `~/audio/.deps` (FCompressor's
`Scripts/deps.sh` fills it), FunkGui from `~/audio/libraries/FunkGui` at the pinned tag (`cmake/Deps.cmake`); a
machine without them fetches them from GitHub instead.

```sh
cmake --workflow --preset dev-verify     # configure, build everything, run every test (build-dev/)
open build-dev/TestingPlayground_artefacts/RelWithDebInfo/Standalone/TestingPlayground.app
```

The Standalone starts with its input muted (JUCE's guard against feedback): unmute it in its Options (the cog in the
title bar), with headphones on. For a host:

```sh
cmake --workflow --preset release-build  # Release with LTO (build-release/)
scripts/install.sh                       # copy the AU and VST3 into ~/Library/Audio/Plug-Ins
```

Builds never install anything by themselves; `scripts/install.sh build-dev` installs a dev build instead.

## Test

```sh
scripts/test.sh                  # build the probes, run them; outputs in build-dev/probe-out/
scripts/test.sh -R dsp.decay -V  # one probe, every row printed
build-dev/TestingPlayground_probes --list
```

| Probe | Checks |
| --- | --- |
| `dsp.render` | renders the impulse response (`dsp.render.ir.wav`, 4 s) and noise bursts and clicks through the reverb (`dsp.render.wav`, with its input); finite, no clipping, the first arrival at the shortest line, the tail's fall, the same bits at blocks of 64 and 333 |
| `dsp.decay` | T60 from the energy decay curve against DECAY at 0.5, 2 and 8 s (within 3 %; measured 0.496, 2.000, 7.996 s), at 4 and 8 lines, 2 and 60 m, 44.1 and 96 kHz; with HF DECAY 50 %, the decay at 250 Hz and 12 kHz against the absorption filters' design (within 5 %) |
| `dsp.stability` | all 256 corners of the parameter range, 10 s of noise each: finite, no growth, the wet level near the input's |
| `dsp.response` | the dry path sample by sample on both channels; the routing (every line's first arrival, sample by sample, at 4, 8 and 16 lines, against a model of the taps, their signs and the DC blocker; the input gain against its law); GAIN linear; no click when GAIN, SIZE, LINES or the LFO shape changes; a mono bus; NaN, inf and huge input samples; DC kept out of the network |
| `dsp.modulation` | each line's read position against RATE, DEPTH, the drawn shape and the line's i/n phase offset, at 4 and 16 lines |
| `dsp.primes` | the prime rule against the legacy table over its whole range (0 to 1,395,629), the legacy test's inputs; every layout's lengths prime, ascending, near their targets, inside the delay lines |
| `plugin.process` | the processor as a host drives it: host writes reach the DSP, a host's NaN is ignored, both channels are the Engine's output bit for bit, an LFO shape set by the editor included |
| `plugin.state` | the host's parameters equal the table; the state (parameters and LFO shape) round-trips; an older state without a shape loads the default; a session of the Visage version restores GAIN; a state value that is not a number loads the default; processBlock before prepareToPlay passes the input through; the tail length hosts are told |
| `gui.dump` | the panel headless to `gui.dump.png`, `gui.dump.paper.png` and `gui.dump.edited.png`; settles, every glyph, a11y, the DELAYS line, a dial drag, every graph gesture (add, move, snap, bend, the end points, remove, the menu) and that each completed one tells the host once; a host's NaN never shows |

A probe is one file in `Tools/probes/<layer>/` whose first line registers it (`// PROBE name=dsp.mytest timeout=60`)
and whose body uses spec rows (`P.near`, `P.le`, `P.ge`, `P.in`, `P.eq`): each states what must hold, with its
tolerance. `Tools/probes/common/Audio.h` has the helpers (sines, levels, a DFT bin, the energy decay curve and its T60
fit, a band-pass, WAV output); `Tools/probes/dsp/Rig.h` runs the Engine as the processor does.

## Swap in your own algorithm

- **Change a piece of this FDN**: the delay lengths in `FdnLayout.cpp` (another spread, another rule than primes),
  the matrix in `Hadamard.h` (any orthogonal matrix keeps the decay exact), the loop in `Fdn.cpp` (`tick()`, one
  sample: reads, absorption, matrix, writes, taps), the interpolation in `DelayLine.h`. Run `scripts/test.sh`:
  `dsp.decay` tells whether the decay still follows DECAY, `dsp.stability` whether any corner grows.
- **Replace it**: keep the `Engine` interface in `Source/dsp/Engine.h` (`prepare`, `reset`, `setParams`,
  `setLfoTable`, `process`, `tailSeconds`) and write your own body, or your own classes under `Source/dsp/` (the
  source lists are folder globs: no CMake edit). The rules of the audio thread: no allocation, locks or I/O in
  `setParams()` and `process()` (allocate in `prepare()`), and smooth every parameter. The core helps:
  `fsdsp::simd` (four float lanes, NEON), `fsdsp::exp2/log2/tanh/sinPi/tanPi` (no libm), `fsdsp::Smoother4`,
  `SmoothRamp`, `ControlTicker`, `Pcg32`, `sanitize`. Then rewrite the probes that measure this reverb
  (`dsp.decay`, `dsp.response`, `dsp.modulation`, `dsp.stability`, `dsp.render`'s first-arrival row,
  `plugin.process`, `plugin.state`'s tail rows); `gui.dump` and
  `plugin.state` work from the parameter table and keep working. Without modulation, drop RATE and DEPTH, and the
  graph with its `EditorPort` (or keep it to drive something else: it hands the Engine a 257-point table).
- **Add a parameter**: one row in `Source/dsp/Params.h` (id, name, label, unit, range, default, centre, control), then
  `values[param::<name>]` in `Engine::setParams`. The host parameter, the saved state and the panel's control follow;
  the panel puts dials and sliders in the top row and choices at its right end (`isModulation()` in
  `Source/editor/Panel.cpp` sends a row to the column beside the graph). Keep IDs forever: hosts store them.

## What is where

```
Source/dsp/              the reverb, plain C++20 without JUCE: Engine (MIX, GAIN, LINES switch), Fdn (the network),
                         FdnLayout and Primes (delay lengths), Hadamard, DelayLine, LfoShape; Params.h (the table)
Source/fsdsp/core/       FSynth's DSP core, vendored at the tag in SEED.tsv (scripts/sync-dsp-core.sh)
Source/plugin/           the JUCE AudioProcessor: parameters from the table, the LFO shape and its state, the Engine
                         called once per block; EditorPort.h, what the editor reaches besides the parameters
Source/editor/           the FunkGui panel (Panel.cpp), the graph (ShapeView.cpp), Theme.h; gpu/ starts it in the plugin
Tools/probes/            tests that measure: dsp/ (the reverb alone), plugin/, gui/ (the panel, headless)
scripts/                 plugin.py, test.sh, sync-dsp-core.sh, install.sh
cmake/Product.cmake      the identity: name, kind (an effect), manufacturer and plugin codes, bundle id, namespace
```

## From the Visage version

`git show legacy-visage:<path>` reads the old files. The legacy plugin ran only a gain (`Gain.h`, -24 to +24 dB);
its FDN was begun and not finished: `FDNConfig::calculate(sample_rate, room_size, ms_decay)` stored its inputs, and
the matrix order, the delay lengths and a "differential decay" were declared and never computed or used. This port
keeps what was there and completes it in the same family (HardwareReverb's, where the legacy delay line came from):

- kept: the delay line (Thiran allpass reads), the prime rule (`findClosestPrime`; the 966 KB table of 106,782
  primes is replaced by trial division, with the same answers over the table's range, checked by `dsp.primes`), the
  parameters it named (room size, the -60 dB time, the matrix order, a differential decay), GAIN with its range and ID;
- added: the delay-length scheme, the Hadamard matrix, the decay gains, the absorption filters, the input and output
  taps, the level normalisation, MIX, the smoothing and the LINES switch;
- fixed: the delay line's integer delays (the legacy split put the allpass pole on the unit circle there);
- GAIN: the same ID, range, default and plain dB value, so a session saved by the Visage version (its state's root
  tag was `PARAMS`; this version writes `PARAMETERS`) restores GAIN. What changed: it is smoothed over 20 ms (the
  core's one-pole, as every parameter here) where the legacy `Gain.h` used 10 ms; it no longer snaps to 0.01 dB
  steps; and the host shows its unit in the value's text ("-6.0 dB") instead of a separate "dB" label;
- the legacy graph editor (`gui/GraphEditor`, 1,486 lines of Visage: File and Edit menus, .lfo files, a strip of
  several graphs, an LFO / ONESHOT switch and a side panel) became `Source/editor/ShapeView.cpp`, its point canvas
  with the same gestures and curve maths, now driving the reverb's modulation; the menus, files, graph strip and
  side panel were left out (the shape is saved with the plugin; RATE and DEPTH are host parameters). Its default
  shape changed: the legacy reset to a single bump (0.5, up to 1 at the middle, back to 0.5); here the default, and
  what Reset shape restores, is a bipolar triangle (0, +1 at a quarter, -1 at three quarters, 0), so the delays move
  both ways around their primes.

## Make a new plugin

Start new plugins from FunkPluginTemplate (`../FunkPluginTemplate/scripts/plugin.py clone --name MyPlugin`, effect or
instrument). This project's own `scripts/plugin.py clone --name MyReverb --code Myrv` copies the playground, FDN and
all, into a new project with its own identity, for a second experiment; `--type instrument`, `--starter` and
`starter-diff` refuse here with one line, before anything is asked or written, because the starters live in the
template. `scripts/plugin.py rename` renames this project in place.

## Update the DSP core

```sh
scripts/sync-dsp-core.sh <fsynth-tag>     # e.g. fsyn-s3 (the seed, SEED.tsv) or a later fsyn-sN
```

It takes `Source/fsdsp/core` from FSynth at that tag (`git tag` in FSynth lists them) and records it in SEED.tsv.
It refuses to overwrite a core file you edited (`--force` overwrites). Build and test after a sync.

## Theme, zoom, diagnostics

The THEME cells (header) and the ZOOM cells (footer) are machine-wide preferences, stored by FunkGui in
`~/Library/Application Support/TestingPlayground/`. Colours are tokens: `Source/editor/Theme.h` shows how to give
this plugin its own accent; the LFO curve uses the theme's `signal` colour. `TSTN_UI_THEME=1` opens the Standalone on
the light theme without saving it, `TSTN_GPU_LOG=1` logs the GPU surface (the prefix is the product's,
`cmake/Product.cmake`).
