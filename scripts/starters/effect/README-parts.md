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
plain parameter values, `process()` with the channels to process in place. Rewrite its body, or add your own classes
under `Source/dsp/` and call them from it. The rules of the audio thread: no allocation, locks or I/O in
`setParams()` and `process()` (allocate in `prepare()`), and smooth every parameter so nothing steps.
The core helps: `fsdsp::simd` (four float lanes, NEON), `fsdsp::exp2/log2/tanh/sinPi/tanPi` (no libm),
`fsdsp::phase` (exact oscillators), `fsdsp::Smoother4`, `SmoothRamp`, `Pcg32`, `sanitize`. Each header says what it
guarantees.

The processor (`Source/plugin`) hands the Engine what a host sends, made safe: at most `prepare()`'s block size per
`process()` call (a larger host block comes in parts, so keep the output independent of how blocks are split), the
last good value of a parameter a host set to NaN, nothing at all before the host's first `prepareToPlay` (the input
then passes through, cleaned of NaN and inf). When a host bypasses the plugin the output crossfades to the input
over 20 ms, and back when the bypass ends, so neither edge clicks; the Engine keeps running on the input meanwhile
(its output unheard), so it resumes from a state that followed the input, not one frozen when the bypass began. An
algorithm with latency must delay the input by it for that crossfade (`Source/plugin/Processor.cpp`).
<!-- /starter:algorithm -->

<!-- starter:probes -->
| Probe | Checks |
| --- | --- |
| `dsp.render` | renders a sweep through the effect to `dsp.render.wav` (and the input); finite, no clipping, block-size invariant |
| `dsp.response` | each channel against a double-precision model of the effect, sample by sample (a swapped, missing or inverted channel fails); gain and tone against their formulas, within 0.02 and 0.05 dB; the smoothing of a gain step |
| `plugin.process` | the processor as a host drives it: parameters the host writes reach the DSP (GAIN +12 dB brings -12 dBFS to 0 dBFS), both channels are the Engine's output bit for bit, a NaN from the host is ignored; a block of 0 samples changes nothing; a block larger than announced reaches the Engine in parts, with the same output; the bypass passes the input through while the Engine keeps running; bypass and un-bypass mid-stream crossfade over 20 ms with no step beyond the signals' own (where a hard switch steps 11 times as far), then give the input and the never-bypassed output, bit for bit |
| `gui.dump` | the panel headless to `gui.dump.png` and `gui.dump.paper.png`; settles, every glyph, a11y; the header clear of the THEME cells with this name and with names of 24, 48 and 64 letters (`gui.dump.longname.png`); a drag writes the parameter; a host's NaN shows the last value |
| `plugin.state` | the host's parameters equal the table; the state round-trips; a NaN's host text is a dash; a state value that is not a number loads the default; processBlock and processBlockBypassed before prepareToPlay pass the input through, sanitised; processBlock gives finite output |
<!-- /starter:probes -->
