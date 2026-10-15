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
plain parameter values, `process()` with the block's note events (`Source/dsp/Events.h`: note-on, note-off,
all-notes-off, each at its sample offset), writing the channels. Rewrite its body, or add your own classes under
`Source/dsp/` and call them from it. The rules of the audio thread: no allocation, locks or I/O in `setParams()` and
`process()` (allocate in `prepare()`), and nothing steps: smooth every parameter, ramp a reused voice (a retrigger's
level, a steal's fade-out before its new note).
The core helps: `fsdsp::simd` (four float lanes, NEON), `fsdsp::exp2/log2/tanh/sinPi/tanPi` (no libm),
`fsdsp::phase` (exact oscillators), `fsdsp::Smoother4`, `SmoothRamp`, `Pcg32`, `sanitize`. Each header says what it
guarantees.

The processor (`Source/plugin`) hands the Engine what a host sends, made safe, and never leaves a note held: MIDI
note-on and note-off (a note-on at velocity 0 is a note-off) and the controllers All Sound Off and All Notes Off
(both release every note) become events; a block of 0 samples still brings its events (apply them: render nothing);
a block larger than `prepare()`'s block size, or with more than 1,024 events, comes in parts (so keep the output
independent of how blocks are split); a parameter a host set to NaN keeps its last good value. When a host bypasses
the plugin the output fades out over 20 ms, and back in when the bypass ends, so neither edge clicks; meanwhile the
Engine runs on the note-offs only: a note let go during the bypass is not held afterwards, and a note held through it
keeps sounding, unheard, and fades back in (its key is still down). Before the host's first `prepareToPlay` the
Engine is not called and the output is silent.
<!-- /starter:algorithm -->

<!-- starter:probes -->
| Probe | Checks |
| --- | --- |
| `dsp.render` | plays an arpeggio and a chord (sine, then triangle) to `dsp.render.wav`; finite, no clipping, block-size invariant |
| `dsp.response` | pitch within 0.005 Hz; level, GAIN and velocity within 0.02 dB; the attack and release shapes; each of eight held notes in the output; voice stealing; no click when a voice is retriggered or stolen |
| `plugin.process` | the processor as a host drives it: a note-on sounds from its sample offset at its velocity, the note-off releases it to silence with no voice left, GAIN written by the host reaches the DSP, a NaN from the host is ignored, both channels are the Engine's output bit for bit. No note is left held by All Notes Off or All Sound Off, a note-on at velocity 0, a note-off in a block of 0 samples, a block of more than 1,024 events, or a note-off while the host bypasses the plugin (whose output is then silent); a block larger than announced reaches the Engine in parts, with the same output; bypass and un-bypass with a note held fade over 20 ms with no step beyond the note's own (where a hard switch steps 17 times as far), then give silence and the never-bypassed output, bit for bit |
| `gui.dump` | the panel headless to `gui.dump.png` and `gui.dump.paper.png`; settles, every glyph, a11y; the header clear of the THEME cells with this name and with names of 24, 48 and 64 letters (`gui.dump.longname.png`); a drag writes the parameter; a host's NaN shows the last value |
| `plugin.state` | the host's parameters equal the table; the state round-trips; a NaN's host text is a dash; a state value that is not a number loads the default; processBlock and processBlockBypassed before prepareToPlay are silent; processBlock gives finite output |
<!-- /starter:probes -->
