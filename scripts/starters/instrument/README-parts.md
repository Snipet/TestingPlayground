# README blocks of the instrument starter

`scripts/plugin.py clone --type instrument` (or `--starter`, for a project that is already an instrument) puts each
block below into the new project's README.md, in place of the block of the same name, with the Source/dsp,
Tools/probes/dsp, Tools/probes/plugin/process.cpp and CLAUDE.md of this folder. Keep the marker lines. The
template's own README.md carries the effect's blocks; `scripts/plugin.py starter-diff` compares them.

<!-- starter:intro -->
As it ships it is an instrument: eight voices, each a sine or a triangle under an attack/release envelope, played
by MIDI, stereo out.

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
| `dsp.render` | plays an arpeggio and a chord (sine, then triangle) to `dsp.render.wav`; finite, no clipping, block-size invariant |
| `dsp.response` | pitch within 0.005 Hz; level, GAIN and velocity within 0.02 dB; the attack and release shapes; each of eight held notes in the output; voice stealing; no click when a voice is retriggered or stolen |
| `plugin.process` | the processor as a host drives it: a note-on sounds from its sample offset at its velocity, the note-off releases it to silence with no voice left, GAIN written by the host reaches the DSP, a NaN from the host is ignored, both channels are the Engine's output bit for bit |
| `gui.dump` | the panel headless to `gui.dump.png` and `gui.dump.paper.png`; settles, every glyph, a11y, a drag writes the parameter |
| `plugin.state` | the host's parameters equal the table; the state round-trips; processBlock gives finite output |
<!-- /starter:probes -->
