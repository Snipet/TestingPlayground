# README blocks of the effect starter

`scripts/plugin.py clone --type effect` (or `--starter`, for a project that is already an effect) puts each
block below into the new project's README.md, in place of the block of the same name, with the Source/dsp,
Tools/probes/dsp, Tools/probes/plugin/process.cpp and CLAUDE.md of this folder. Keep the marker lines. The
template's own README.md carries the effect's blocks; `scripts/plugin.py starter-diff` compares them.

<!-- starter:intro -->
As it ships it is an effect: a gain and a tone control (a one-pole low-pass), stereo in and out.

The editor is drawn from the parameter table: a dial or a slider per parameter, the theme and zoom cells. After a
build, `build-dev/probe-out/gui.dump.png` is a picture of it (the `gui.dump` probe renders it headless).
<!-- /starter:intro -->

<!-- starter:where -->
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
<!-- /starter:where -->

<!-- starter:algorithm -->
## Change the algorithm

`Source/dsp/Engine.cpp` is the whole algorithm: `prepare()` before playback, `setParams()` once per block with the
plain parameter values, `process()` once per block (an effect processes the channels in place; an instrument writes
them, playing the block's note events at their offsets). Rewrite its body, or add your own classes under
`Source/dsp/` and call them from it. The rules of the audio thread: no allocation, locks or I/O in `setParams()` and
`process()` (allocate in `prepare()`), and smooth every parameter so nothing steps. The core helps: `fsdsp::simd`
(four float lanes, NEON), `fsdsp::exp2/log2/tanh/sinPi/tanPi` (no libm), `fsdsp::phase` (exact oscillators),
`fsdsp::Smoother4`, `SmoothRamp`, `Pcg32`, `sanitize`. Each header says what it guarantees.
<!-- /starter:algorithm -->

<!-- starter:probes -->
| Probe | Checks |
| --- | --- |
| `dsp.render` | renders a sweep through the effect to `dsp.render.wav` (and the input); finite, no clipping, block-size invariant |
| `dsp.response` | each channel against a double-precision model of the effect, sample by sample (a swapped, missing or inverted channel fails); gain and tone against their formulas, within 0.02 and 0.05 dB; the smoothing of a gain step |
| `plugin.process` | the processor as a host drives it: parameters the host writes reach the DSP (GAIN +12 dB brings -12 dBFS to 0 dBFS), both channels are the Engine's output bit for bit, a NaN from the host is ignored |
| `gui.dump` | the panel headless to `gui.dump.png` and `gui.dump.paper.png`; settles, every glyph, a11y, a drag writes the parameter |
| `plugin.state` | the host's parameters equal the table; the state round-trips; processBlock gives finite output |
<!-- /starter:probes -->
