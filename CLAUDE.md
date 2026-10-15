# TestingPlayground: rules for a Claude session

A research playground for feedback delay network reverbs: an effect plugin (AU, VST3, Standalone; macOS arm64) on
JUCE 8.0.4 with a FunkGui editor and FSynth's DSP core (vendored), made from FunkPluginTemplate. Change the algorithm,
the parameters and the panel freely; keep the rules below. README.md is the user's guide (the algorithm, the editor,
the probes); this file is what a session must know (`scripts/plugin.py info` prints the identity).

<!-- origin -->
The reverb is the legacy (tag `legacy-visage`) FDN, ported and completed; the processor also loads that version's
sessions. Shared fixes from FunkPluginTemplate are carried over by hand (`Source/plugin`, `Source/editor`, the plugin
and gui probes, `scripts/plugin.py`), keeping this project's own changes.
<!-- /origin -->

## What goes where

| Path | What | Rule |
| --- | --- | --- |
| `cmake/Product.cmake` | identity: name, kind (an effect), codes, bundle id, namespace (`testingplayground`), environment prefix | `scripts/plugin.py` rewrites it; the plugin code stays unique among the user's plugins |
| `Source/fsdsp/core/**` | FSynth's DSP core, vendored (`SEED.tsv`) | do not edit casually: `scripts/sync-dsp-core.sh` refuses to overwrite edits |
| `Source/dsp/**` | the reverb: `Engine` (MIX, GAIN, the LINES switch), `Fdn` (the network, one `tick()` per sample), `FdnLayout` + `Primes` (delay lengths), `Hadamard`, `DelayLine` (Thiran reads), `LfoShape` (the graph's data and table), `Params.h` (the table) | JUCE-free C++20 |
| `Source/plugin/**` | the JUCE `AudioProcessor`: APVTS from the table, one FunkGui port per parameter, the LFO shape (kept under a mutex, saved as `<LFO>` in the state, its table published to the audio thread through `fsdsp::Seqlock`), the bypass; `EditorPort.h` | reads parameters once per block, calls the Engine; copes with what a host may do (below) |
| `Source/editor/**` | the FunkGui `Panel` (from the table, plus the modulation section), `ShapeView` (the LFO graph), `ParamModel`, `Theme.h`; `gpu/` the EditorHost factory | reaches the processor only through its ports and `EditorPort` |
| `Tools/probes/**` | probes on FunkGui's Harness: `dsp/` (Source/dsp only), `plugin/`, `gui/` | spec rows only, no golden files |

There are no starters here (`scripts/starters/` is the template's): `plugin.py clone` copies this playground with its
FDN (its plugin code must be free across `~/audio/plugins`; the probes' namespace alias follows the new name; the
`<!-- origin -->` blocks are left out); `--type instrument`, `--starter` and `starter-diff` refuse. New plugins start
from FunkPluginTemplate.

## Build and test

- `cmake --workflow --preset dev-verify`: configure, build (Standalone, AU, VST3, probes) and run every test in
  `build-dev`. Done means it exits 0 with no warning from our sources (they build with `-Werror`).
- `scripts/test.sh`: build and run the probes only (`-R <name>`, `-V` pass through to ctest). Outputs land in
  `build-dev/probe-out/`: `dsp.render.ir.wav` (the impulse response), `dsp.render.wav` and its input, and the editor
  rendered headless (`gui.dump.png`, `gui.dump.paper.png`, `gui.dump.edited.png`; `gui.dump.longname.png`, its header
  with a name of 64 letters). Look at the PNGs after any editor change. `dsp.stability` runs 256 settings for 10 s each (about 20 s).
- When the algorithm changes, change its probes with it: `dsp.decay` (T60 against DECAY, by band against HF DECAY),
  `dsp.response` (the dry path and every line's first arrival against exact values, sample by sample, with the input
  gain's law and the tap signs it copies from `Fdn.cpp`; click checks above 4 kHz; NaN input; DC), `dsp.modulation`
  (each line's read position against RATE, DEPTH and the shape), `dsp.stability` (every corner), `dsp.primes` (the
  delay-length rule), `dsp.render`, `plugin.process` (the processor as a host drives it, bit for bit against the
  Engine; odd block sizes; the bypass edges), `plugin.state`'s tail rows. State tolerances that say what must hold, and why.
- `cmake --workflow --preset release-build`: Release with LTO in `build-release`. `scripts/install.sh` copies the AU
  and VST3 into `~/Library/Audio/Plug-Ins`; only the user (or the lead) runs it, never an agent.
- The presets build at `-j6` (the machine is shared). No network: JUCE, bgfx and shaderc come from `~/audio/.deps`
  (read-only, never write there) and FunkGui is cloned from `~/audio/libraries/FunkGui` at the pinned tag.

## Layer and real-time rules

- `Source/dsp` and `Source/fsdsp` include nothing from JUCE, FunkGui, `plugin/` or `editor/` (the test
  `lint.layers` checks; the DSP library has no JUCE include path). `Source/plugin/EditorPort.h` stays JUCE-free too:
  the editor includes it.
- `Engine::setParams`, `setLfoTable` and `process` run on the audio thread: no allocation, no locks, no I/O, no
  logging. Allocate in `prepare()` (the delay lines are sized there for every SIZE at that rate). Smooth every
  parameter (the core's `Smoother4`, `SmoothRamp`, `LinearRamp`): nothing steps. A change that cannot be smoothed
  (LINES) fades the wet path out, switches while it is silent and fades back in. The processor hands `setParams`
  finite values within the table's range (a host's NaN keeps the last good value; the panel shows the last finite
  value, `shownValue01` in `ParamModel.h`, and a state's NaN loads the default). Before `prepare()`, `process()` only
  sanitises and passes the input through.
- `Engine::process` gets at most `prepare()`'s `maxBlockSize` samples per call (the processor cuts a larger host
  block into parts), sometimes 0. Keep the output independent of how the blocks are split (`dsp.render` checks 64
  against 333 samples; `plugin.process` checks the parts).
- The processor (`Source/plugin`) copes with what a host may do: the bypass (`processBlockBypassed`) crossfades
  between the Engine's output and the input over 20 ms each way (smootherstep, equal-gain), and the Engine keeps
  running on the input while bypassed (its parameters and LFO table too), so un-bypassing resumes from a current state
  (an Engine with latency must delay that dry path by it; the FDN has none); a block before any `prepareToPlay`
  passes the input through, sanitised, without calling the Engine; a host's NaN keeps the last good value (and its
  text is a dash); a state value that is not a number loads as the parameter's default. `plugin.process` and
  `plugin.state` check each.
- The loop's decay must stay exact: any matrix must be orthogonal and any interpolator unity-gain, or a corner grows
  (`dsp.stability` catches a 0.2 % energy gain in the matrix).
- `Engine::tailSeconds()` is called by hosts from any thread: an atomic that `setParams` stores.
- Prefer the core's math on the audio path (`fsdsp::exp2/log2/tanh/sinPi/cosPi/tanPi`, `fsdsp::simd`), and keep the
  output independent of the host's block size: per-sample state, coefficient updates on the absolute 16-sample grid
  (`fsdsp::ControlTicker`); `dsp.render` checks 64 against 333 samples.
- The LFO shape is message-thread data (`LfoShape`, rendered with the standard library's math) handed to the audio
  thread as a table (`Processor::storeShape`, a Seqlock read in `readParameters` each block, bypassed or not, a
  20 ms crossfade in the Fdn).
- Flags: `-ffp-contract=off`, never `-ffast-math`. Product constants come from the generated `Product.h`, never from
  `JucePlugin_*`. The editor never uses JUCE's parameter attachments: widgets write through FunkGui's
  `GestureController` to the ports (`funkgui::JuceParamPort`); the graph writes through `EditorPort` and calls
  `endShapeEdit()` once an edit is complete, so the host knows the state changed.

## Recipes

- **A parameter**: add an index and a row in `Source/dsp/Params.h` (id, name, label, unit, range, default, centre,
  control: dial, slider, choice or hidden); read `values[param::<name>]` in `Engine::setParams`. The processor, the
  host, the saved state and the panel follow (dials and sliders in the top row, choices at its right end;
  `isModulation()` in `Panel.cpp` puts a row beside the graph). Never change an ID once sessions use it.
- **An algorithm**: change the pieces under `Source/dsp/` or replace the Engine's body (keep its interface); new files
  need no CMake edit (folder globs). README.md, "Swap in your own algorithm", lists which probes then change.
- **A view**: add an `Item` in `Source/editor/Panel.cpp` (a FunkGui widget, or your own drawing on the
  `funkgui::Canvas`, as `ShapeView` does, under a tag of its own from `ui::kHeaderTextTag + 1` when a probe needs to
  find it) and place it in the constructor; then check `gui.dump`'s PNGs. The header fits any product name (the
  panel widens, then drops the kind and version, then cuts the name: `gui.dump`'s HEADER rows).
- **A probe**: a file `Tools/probes/<layer>/<name>.cpp` whose first line is `// PROBE name=<layer>.<name> timeout=<s>`
  and whose body is `PROBE(<layer>, <name>) { ...; return P.finish(); }`. Use spec rows (`P.near`, `P.le`, `P.ge`,
  `P.in`, `P.eq`); helpers are in `Tools/probes/common/Audio.h` (levels, DFT, T60 from the energy decay curve,
  band-pass, WAV) and `Tools/probes/dsp/Rig.h` (the Engine run as the processor runs it).

## Git

Work on a branch; commit messages say what changed and why. Never commit build directories (`build-*/` is ignored).
Text about this repository's own history goes in `<!-- origin -->` blocks (README.md, CLAUDE.md): a clone has no
such history, and `plugin.py clone` leaves them out.
