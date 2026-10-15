# TestingPlaygroundSynth: rules for a Claude session

An audio plugin (AU, VST3, Standalone; macOS arm64) on JUCE 8.0.4 with a FunkGui editor and FSynth's DSP core
(vendored). It is a research playground: change the algorithm, the parameters and the panel freely, keep the rules
below. README.md is the user's guide; this file is what a session must know. This project is an instrument
(`scripts/plugin.py info` prints its identity).

## What goes where

| Path | What | Rule |
| --- | --- | --- |
| `cmake/Product.cmake` | identity: name, kind (effect, instrument), codes, bundle id, namespace | `scripts/plugin.py` rewrites it; the plugin code stays unique among the user's plugins |
| `Source/fsdsp/core/**` | FSynth's DSP core, vendored (`SEED.tsv`) | do not edit casually: `scripts/sync-dsp-core.sh` refuses to overwrite edits |
| `Source/dsp/**` | the algorithm: `Params.h` (the parameter table), `Engine.h/.cpp` | JUCE-free C++20, namespace `testingplaygroundsynth` |
| `Source/plugin/**` | the JUCE `AudioProcessor`: APVTS from the table, one FunkGui port per parameter, MIDI to `Event`s, the bypass | reads parameters once per block, calls the Engine; never loses a note-off; the same for both kinds |
| `Source/editor/**` | the FunkGui `Panel` (built from the table), `ParamModel`, `Theme.h`, `EnvelopeView` (an example view, shown when the table has ATTACK, DECAY, SUSTAIN and RELEASE); `gpu/` the EditorHost factory | reaches the processor only through its ports |
| `Tools/probes/**` | probes on FunkGui's Harness: `dsp/` (Source/dsp only), `plugin/`, `gui/` | spec rows only, no golden files |
| `scripts/starters/<kind>/` | the effect and instrument starters: `Source/dsp`, `Tools/probes/dsp`, `Tools/probes/plugin/process.cpp`, `CLAUDE.md`, `README-parts.md` (README.md's `<!-- starter:... -->` blocks) | what `plugin.py clone --type/--starter` installs; this project's own copies began as its kind's starter (`plugin.py starter-diff` shows what changed since) |

## Build and test

- `cmake --workflow --preset dev-verify`: configure, build (Standalone, AU, VST3, probes) and run every test in
  `build-dev`. Done means it exits 0 with no warning from our sources (they build with `-Werror`).
- When the algorithm changes, change its probes with it (`Tools/probes/dsp/`, `Tools/probes/plugin/process.cpp`):
  check both channels against a model or a formula sample by sample (a level alone misses a flipped polarity), and
  drive the processor as a host does (parameter writes, MIDI at offsets, odd block sizes, a bypass), not only the
  Engine.
- `scripts/test.sh`: build and run the probes only (`-R <name>`, `-V` pass through to ctest). Outputs land in
  `build-dev/probe-out/`: `dsp.render*.wav`, and `gui.dump.png` / `gui.dump.paper.png`, the editor rendered headless
  (`gui.dump.longname.png`: its header with a name of 64 letters). Look at the PNGs after any editor change.
- `cmake --workflow --preset release-build`: Release with LTO in `build-release`. `scripts/install.sh` copies the AU
  and VST3 into `~/Library/Audio/Plug-Ins`; only the user (or the lead) runs it, never an agent.
- The presets build at `-j6` (the machine is shared). No network: JUCE, bgfx and shaderc come from `~/audio/.deps`
  (read-only, never write there) and FunkGui is cloned from `~/audio/libraries/FunkGui` at the pinned tag.

## Layer and real-time rules

- `Source/dsp` and `Source/fsdsp` include nothing from JUCE, FunkGui, `plugin/` or `editor/` (the test
  `lint.layers` checks; the DSP library has no JUCE include path).
- `Engine::setParams` and `Engine::process` run on the audio thread: no allocation, no locks, no I/O, no logging.
  Allocate in `prepare()`. Smooth every parameter (the core's `Smoother4`, `SmoothRamp`, `LinearRamp`): nothing steps,
  a reused voice included (ramp its level, or fade it out before it restarts).
  The processor hands `setParams` finite values within the table's range (a host's NaN keeps the last good value).
- `Engine::process` gets the block's note events at their sample offsets, at most `prepare()`'s `maxBlockSize`
  samples per call (the processor cuts a larger host block into parts) and at most `kMaxEventsPerBlock` events,
  sometimes 0 samples: apply the events then, render nothing (a note-off is never lost). Keep the output independent
  of how the blocks are split (`dsp.render` checks 64 against 333 samples; `plugin.process` checks the parts).
- The processor (`Source/plugin`, the same for both kinds) copes with what a host may do, and never leaves a note
  held: a note-on at velocity 0 is a note-off; All Sound Off (CC 120) and All Notes Off (CC 123) release every note;
  a block with more than `kMaxEventsPerBlock` events is played in parts; while bypassed (`processBlockBypassed`) the
  output is silent, faded out and back in over 20 ms (smootherstep), and the Engine runs on the note-offs and
  all-notes-offs (no note-ons): a note held through the bypass keeps sounding unheard and fades back in after it. A
  block before any `prepareToPlay` is silent, without calling the Engine; a host's NaN keeps the last good value (and
  its text is a dash); a state value that is not a number loads as the parameter's default. `plugin.process` and
  `plugin.state` check each.
- `Engine::tailSeconds()` is called by hosts from any thread: return a constant, or an atomic that `setParams` stores.
- Prefer the core's math on the audio path (`fsdsp::exp2/log2/tanh/sinPi/cosPi/tanPi`, `fsdsp::simd`).
- Flags: `-ffp-contract=off`, never `-ffast-math`. Product constants come from the generated `Product.h`, never from
  `JucePlugin_*`. The editor never uses JUCE's parameter attachments: widgets write through FunkGui's
  `GestureController` to the ports (`funkgui::JuceParamPort`), which keeps host automation gestures right. A view
  that reads a port shows its last finite value (`ui::shownValue01`), never "nan".

## Recipes

- **A parameter**: add an index and a row in `Source/dsp/Params.h` (id, name, label, unit, range, default, centre,
  control: dial, slider, choice or hidden); read `values[param::<name>]` in `Engine::setParams`. The processor, the
  host, the saved state and the panel follow. Never change an ID once sessions use it.
- **An algorithm**: replace the body of `Source/dsp/Engine.cpp` (keep the `Engine` interface), or add classes under
  `Source/dsp/` (the globs pick up new files: no CMake edit) and call them from the Engine.
- **A widget or a view**: the panel lays out every table row itself. For anything else (a graph, a meter), add an
  `Item` in `Source/editor/Panel.cpp` (a FunkGui widget, or your own drawing on the `funkgui::Canvas` under a tag of
  its own, as `EnvelopeView` does with `ui::kEnvelopeTag`: the next free one is `ui::kEnvelopeTag + 1`) and place it
  in the constructor; then check `gui.dump`'s PNGs.
- **A probe**: a file `Tools/probes/<layer>/<name>.cpp` whose first line is `// PROBE name=<layer>.<name> timeout=<s>`
  and whose body is `PROBE(<layer>, <name>) { ...; return P.finish(); }`. Use spec rows (`P.near`, `P.le`, `P.ge`,
  `P.in`, `P.eq`) with tolerances that say what must hold; helpers are in `Tools/probes/common/Audio.h`.
- **Another plugin**: `scripts/plugin.py clone --name <Name> [--type effect|instrument] [--code <Xxxx>]` (README.md).

## Git

Work on a branch; commit messages say what changed and why. Never commit build directories (`build-*/` is ignored).
Text about this repository's own history goes in `<!-- origin -->` blocks (README.md, CLAUDE.md): a clone has no
such history, and `plugin.py clone` leaves them out.
