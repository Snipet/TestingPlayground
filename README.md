# TestingPlaygroundSynth

A playground for synthesiser research: a polyphonic sine synth with an ADSR, as an AU, VST3 and Standalone for macOS
on Apple silicon, built on JUCE 8.0.4, with a GPU editor made with [FunkGui](https://github.com/Snipet/FunkGui) and
the DSP core of FSynth (SIMD, fast math, phase accumulators, smoothers, random numbers), vendored. Swap in your own
oscillator, envelope or voice policy; the probes measure what changed.

<!-- starter:intro -->
It plays the sine voice synth of the Visage version (tag `legacy-visage`), ported onto the DSP core: sixteen voices,
each a sine under an ADSR at its note's velocity, played by MIDI on the sample each note arrives on, mono to both
output channels. Five parameters: GAIN (-24 to +24 dB), ATTACK (1 to 200 ms), DECAY (1 to 2,000 ms), SUSTAIN (0 to
100 %) and RELEASE (1 to 1,000 ms). At the defaults (SUSTAIN 100 %) it sounds as the legacy synth did.
<!-- /starter:intro -->

The editor is drawn from the parameter table: a picture of the envelope (ENVELOPE), a dial per parameter, the theme
and zoom cells. After a build, `build-dev/probe-out/gui.dump.png` is a picture of it (the `gui.dump` probe renders it
headless; `gui.dump.envelope.png` shows a decaying envelope with the pointer on DECAY).

The Visage version stays in git: `git show legacy-visage:Source/dsp/Voice.h` prints one of its sources, and
`git worktree add ../TestingPlaygroundSynth-legacy legacy-visage` checks the whole of it out beside this one.

## Build and run

Requirements: macOS 14 or later on Apple silicon, Xcode or its command-line tools, CMake 3.30 or later, Ninja,
Python 3. JUCE, bgfx and the shader compiler come from the machine cache `~/audio/.deps` (FCompressor's
`Scripts/deps.sh` fills it), FunkGui from `~/audio/libraries/FunkGui` at the pinned tag (`cmake/Deps.cmake`); a
machine without them fetches them from GitHub instead.

```sh
cmake --workflow --preset dev-verify     # configure, build everything, run every test (build-dev/)
open build-dev/TestingPlaygroundSynth_artefacts/RelWithDebInfo/Standalone/TestingPlaygroundSynth.app
```

In the Standalone, choose the MIDI input (a keyboard) and the audio output in its Options (the cog in the title bar).
For a host:

```sh
cmake --workflow --preset release-build  # Release with LTO (build-release/)
scripts/install.sh                       # copy the AU and VST3 into ~/Library/Audio/Plug-Ins
```

Builds never install anything by themselves; `scripts/install.sh build-dev` installs a dev build instead. The plugin
code is `Tsyn` (manufacturer `Funk`), so it loads beside TestingPlayground (`Tstn`) and the user's other plugins.

## What is where

```
cmake/Product.cmake          the identity: name, kind (instrument), version, codes, bundle id, C++ namespace
Source/dsp/                  the synth, plain C++20 without JUCE (namespace testingplaygroundsynth):
  Params.h                     the parameter table (GAIN, ATTACK, DECAY, SUSTAIN, RELEASE)
  SineOscillator.h             the waveform and the pitch: phase accumulators, four voices per SIMD vector
  Adsr.h                       the envelope, four voices per SIMD vector
  VoiceScheduler.h             which voice plays a note (the legacy policy)
  Engine.h/.cpp                the voices, the note events at their offsets, the ramps, the output gain
Source/fsdsp/core/           FSynth's DSP core, vendored at the tag in SEED.tsv (scripts/sync-dsp-core.sh)
Source/plugin/               the JUCE AudioProcessor: parameters from the table, MIDI to note events, the Engine
Source/editor/               the FunkGui panel, built from the table; EnvelopeView (the ENVELOPE picture); Theme.h
Tools/probes/                tests that measure: dsp/ (the synth alone), plugin/ (the processor), gui/ (the panel)
scripts/                     plugin.py, test.sh, sync-dsp-core.sh, install.sh; starters/ (the template's starters)
CMakeLists.txt, cmake/       the build; every source list is a folder glob, so new files need no CMake edit
```

## How the synth works

- **Notes** reach `Engine::process()` as events with their sample offset in the block (`Source/dsp/Events.h`): it
  renders up to each event, applies it, and carries on, so every note starts and stops on its own sample. The
  processor turns MIDI note-on and note-off (a note-on at velocity 0 is a note-off) and the controllers All Sound Off
  and All Notes Off into these events; like the legacy synth it ignores the rest (pitch bend, sustain pedal).
- **Voices**: sixteen, in four SIMD vectors of four. A voice is a phase accumulator through a sine
  (`SineOscillator.h`), times the ADSR (`Adsr.h`), times its level, 0.25 x velocity (the legacy headroom: one note at
  full velocity peaks at -12 dBFS, four at 0 dBFS). The sum, times GAIN, goes to both channels.
- **Pitch**: 440 x 2^((note - 69) / 12) Hz, within 0.002 cents; the phase is a 32-bit integer that wraps, so a held
  note never drifts.
- **ADSR**, straight lines: the attack rises at 1 / ATTACK per second from wherever the voice is (from silence it lasts
  ATTACK); the decay falls from 1 to SUSTAIN in DECAY; the release falls from wherever the note-off finds it to 0 in
  RELEASE.
- **Voice policy** (the legacy one, `VoiceScheduler.h`): a note already sounding is played again on its own voice; else
  the first silent voice takes it; else the voice of the oldest note-on is stolen. A note-off releases the voice
  playing that note.
- **Nothing steps** (the legacy synth clicked in these places): a stolen voice fades out over 2 ms (a smootherstep)
  and then starts its note, which therefore sounds 2 ms late; a note played again keeps its phase, and its level moves
  to the new velocity's over 2 ms; GAIN and SUSTAIN are smoothed per sample (20 ms); a change of ATTACK, DECAY or
  RELEASE changes a slope, never a level, and reaches the notes already playing.
- The output does not depend on how the host splits its blocks (the `dsp.render` probe compares 64 and 333 samples).

## Swap in your own algorithm

Change one piece at a time and run `scripts/test.sh` after each step; the probes say what still holds.

- **Another waveform**: `SineOscillator4::wave()` in `Source/dsp/SineOscillator.h` maps four phases (a 32-bit
  integer, 2^32 = one cycle) to four samples in [-1, 1]. A waveform with harmonics (saw, square) aliases unless you
  band-limit it. Then adapt `dsp.level` (its MODEL row and its levels are a sine's); `dsp.pitch` measures any wave that
  crosses zero upwards once per cycle.
- **Another envelope**: `adsr::Quad::tick()` in `Source/dsp/Adsr.h` is the whole per-sample law (four voices at once:
  `fsdsp::simd` ops, masks for the gate and the attack); `Bank::start/retrigger/release` are what note-on and note-off
  do. Then adapt `dsp.envelope`, which checks each segment to the sample.
- **Another voice policy**: `VoiceScheduler::noteOn()` decides retrigger, start or steal; the Engine carries it out.
  Then adapt `dsp.voices`.
- **Something else entirely** (FM, a filter per voice, noise): the per-sample loop is `Engine::renderSpan()`; keep the
  `Engine` interface (`prepare`, `setParams`, `process`, `tailSeconds`) and the processor, the editor and
  `plugin.process` keep working. Allocate only in `prepare()`; no locks, I/O or allocation in `setParams()` and
  `process()`; ramp anything that would step.
- **A new parameter**: one row in `Source/dsp/Params.h` (below), then read `values[param::<name>]` in
  `Engine::setParams()`.

The DSP core's headers say what each function guarantees: `fsdsp::simd` (four float lanes, NEON),
`fsdsp::exp2/log2/tanh/sinPi/tanPi` (no libm on the audio thread), `fsdsp::phase`, `fsdsp::Smoother4`,
`fsdsp::smootherstep`, `fsdsp::Pcg32` (random numbers for noise or detune), `fsdsp::sanitize`.

## Add a control

A parameter is one row of the table in `Source/dsp/Params.h`:

```cpp
namespace param { enum Index : std::size_t { gain, /* ... */ detune, count }; }     // 1. an index, before count

{ .id = "detune", .name = "Detune", .label = "DETUNE", .unit = Unit::none,          // 2. its row, at the same place
  .min = 0.0f, .max = 50.0f, .def = 0.0f, .centre = 25.0f, .control = Control::dial },
```

then `values[param::detune]` in `Engine::setParams` (3). The host parameter, the saved state, the panel's control and
its text follow from the row: `control` is `dial`, `slider`, `choice` (cells, with `.choices`/`.numChoices`) or
`hidden`; `centre` is the value at the middle of the control's travel (the midpoint for linear, lower to give the low
end more room, as for times and frequencies). Keep IDs forever: hosts store them in sessions.

The ENVELOPE picture is the example of a view of your own: `Source/editor/EnvelopeView.cpp` reads four parameters
through their ports and draws on the `funkgui::Canvas` (an area and a line); `Panel.cpp` places it and draws it. For
an interactive view, add an `Item` in `Source/editor/Panel.cpp` (FunkGui widgets: `Dial`, `RuleSlider`,
`SegmentedSelector`, `LatchToggle`, ...). The panel is GPU-free: the plugin runs it on Metal, the `gui.dump` probe on
the CPU, so look at the PNGs after an editor change.

## Test

```sh
scripts/test.sh                  # build the probes, run them; outputs in build-dev/probe-out/
scripts/test.sh -R dsp.voices -V # one probe, every row printed
build-dev/TestingPlaygroundSynth_probes --list
build-dev/TestingPlaygroundSynth_probes dsp.pitch
```

<!-- starter:probes -->
| Probe | Checks |
| --- | --- |
| `dsp.pitch` | a held A4 at 44.1, 48 and 96 kHz and eleven notes from A0 to C8, from their zero crossings: within 0.5 cent (they measure under 0.001); no drift while held |
| `dsp.level` | one note at -12.04 dBFS, velocity and GAIN within 0.02 dB; a four-note chord, each note and the RMS; sixteen notes at once, each in the mix; a note sample by sample against a model in double |
| `dsp.envelope` | each ADSR segment to the sample (the note starts on its offset, the release on its note-off); GAIN, SUSTAIN and RELEASE changed mid-note never step |
| `dsp.voices` | sixteen notes on sixteen voices; the seventeenth steals the oldest after a 2 ms fade; a note played again keeps its voice; a note-off releases the right voice; the freed voice is reused; no click on a steal or a retrigger |
| `dsp.render` | a short phrase (a melody over chords, ending with more notes than voices) to `dsp.render.wav`; finite, no clipping, block-size invariant |
| `plugin.process` | the processor as a host drives it: notes start on their sample offsets inside a block, at their velocity; GAIN written by the host reaches the DSP; a NaN from the host is ignored; the note-off leaves silence and no voice; the output is the Engine's bit for bit |
| `gui.dump` | the panel headless to `gui.dump.png`, `gui.dump.paper.png` and `gui.dump.envelope.png`; settles, every glyph, a11y, a drag writes the parameter; the envelope follows the parameters and highlights the segment under the pointer |
| `plugin.state` | the host's parameters equal the table; the state round-trips; processBlock gives finite output |
<!-- /starter:probes -->

`dsp.*` and `plugin.process` test the synth: change them with it. `gui.dump` and `plugin.state` work from the
parameter table, whatever the algorithm. `cmake --workflow --preset dev-verify` runs them all with the build, plus
`lint.layers` (nothing in `Source/dsp` or `Source/fsdsp` includes JUCE, FunkGui, `plugin/` or `editor/`).

A probe is one file in `Tools/probes/<layer>/` whose first line registers it (`// PROBE name=dsp.mytest timeout=60`)
and whose body uses spec rows (`P.near`, `P.le`, `P.ge`, `P.in`, `P.eq`): each states what must hold, with its
tolerance. `Tools/probes/common/Audio.h` has the helpers (sines, levels, a DFT bin, frequency, WAV output);
`Tools/probes/dsp/Rig.h` plays a score of notes through the Engine as the processor would.

## Make a new plugin

```sh
scripts/plugin.py clone --name MySynth --code Mysy                     # this synth, as a new plugin (../MySynth)
scripts/plugin.py clone --name MySynth --code Mysy --starter           # the template's minimal instrument instead
scripts/plugin.py clone --name MyDelay --type effect --code Mydl       # an effect, from the template's starter
```

The clone gets its own name, plugin code, bundle id and C++ namespace (asked for, or derived from the name), keeps
the manufacturer code, and builds without edits. It refuses a plugin code another project beside it already uses: two
plugins with one code hide each other in a host. `--type` switches the kind, and `--starter` restarts from the
template's starter of the same kind (`scripts/starters/`): the algorithm, its probes and this README's kind-specific
blocks (the ones between `<!-- starter:... -->` markers). `plugin.py rename` renames this project in place;
`plugin.py starter-diff` shows how this synth differs from the template's instrument starter.

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
