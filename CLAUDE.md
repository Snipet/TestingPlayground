# TestingPlaygroundSynth: rules for a Claude session

A research playground for synthesiser voices: a polyphonic sine synth with an ADSR (AU, VST3, Standalone; macOS
arm64) on JUCE 8.0.4, with a FunkGui editor and FSynth's DSP core (vendored). Change the algorithm, the parameters and
the panel freely; keep the rules below. README.md is the user's guide (how the synth works, how to swap in an
algorithm); this file is what a session must know.

<!-- origin -->
It is the legacy Visage synth (the tag `legacy-visage`; never move it) ported onto that stack, made from
FunkPluginTemplate with `scripts/plugin.py`; README.md lists what changed from the legacy synth. Its plugin code is
new: the legacy code `Tstn`, which it shared with TestingPlayground (the original project), stays TestingPlayground's.
<!-- /origin -->

## What goes where

| Path | What | Rule |
| --- | --- | --- |
| `cmake/Product.cmake` | identity: name, kind (instrument), codes, bundle id, namespace (`scripts/plugin.py info` prints them) | `scripts/plugin.py` rewrites it; the plugin code stays unique among the user's plugins |
| `Source/fsdsp/core/**` | FSynth's DSP core, vendored (`SEED.tsv`) | do not edit casually: `scripts/sync-dsp-core.sh` refuses to overwrite edits |
| `Source/dsp/Params.h` | the parameter table: GAIN, ATTACK, DECAY, SUSTAIN, RELEASE | IDs are forever (hosts store them) |
| `Source/dsp/SineOscillator.h` | the waveform (`wave()`) and the pitch (uint32 phase accumulators), four voices per SIMD vector | JUCE-free C++20, namespace `testingplaygroundsynth` (all of `Source/dsp`) |
| `Source/dsp/Adsr.h` | the linear ADSR, four voices per SIMD vector (`Quad::tick`), and the per-voice state (`Bank`) | |
| `Source/dsp/VoiceScheduler.h` | the voice policy (the legacy one): retrigger the same note, else the first silent voice, else steal the oldest note-on | decides only; the Engine carries it out |
| `Source/dsp/Engine.h/.cpp` | 16 voices, note events at their sample offsets, the 2 ms retrigger and steal ramps, GAIN and SUSTAIN smoothing | keep its interface: the processor and `plugin.process` use it |
| `Source/fminus/**` | FMinus, the JIT compiler (`~/audio/libraries/FMinus`, `cmake/Deps.cmake`): `FMinusProgram` compiles FMinus source and gives its functions as C function pointers; library `<name>_fminus`, which `<name>_dsp` links | JUCE-free; FMinus's and LLVM's headers only in its `.cpp` (`lint.layers`), so the rest keeps C++20 |
| `Source/plugin/**` | the JUCE `AudioProcessor`: APVTS from the table, one FunkGui port per parameter, MIDI to `Event`s, the bypass | reads parameters once per block, calls the Engine; never loses a note-off; the same for both kinds |
| `Source/editor/**` | the FunkGui `Panel` (built from the table: ENVELOPE, then the dials in a row spread to the width), `EnvelopeView` (the ADSR picture, from the ports), `ParamModel`, `Theme.h`; `gpu/` the EditorHost factory | reaches the processor only through its ports |
| `Tools/probes/**` | probes on FunkGui's Harness: `dsp/` (Source/dsp only: pitch, level, envelope, voices, render), `plugin/`, `gui/`, `fminus/` (FMinusProgram) | spec rows only, no golden files |
| `scripts/starters/<kind>/` | the template's effect and instrument starters: `Source/dsp`, `Tools/probes/dsp`, `Tools/probes/plugin/process.cpp`, `CLAUDE.md`, `README-parts.md` (README.md's `<!-- starter:... -->` blocks) | what `plugin.py clone --type/--starter` installs; not this synth |

## Build and test

- `cmake --workflow --preset dev-verify`: configure, build (Standalone, AU, VST3, probes) and run every test in
  `build-dev`. Done means it exits 0 with no warning from our sources (they build with `-Werror`).
- `scripts/test.sh`: build and run the probes only (`-R <name>`, `-V` pass through to ctest). Outputs land in
  `build-dev/probe-out/`: `dsp.render.wav` (a short phrase), and `gui.dump.png`, `gui.dump.paper.png`,
  `gui.dump.envelope.png`, `gui.dump.longname.png` (the header with a name of 64 letters), the editor rendered
  headless. Look at the PNGs after any editor change.
- When the synth changes, change its probes with it: `dsp.pitch` (zero crossings, within 0.5 cent), `dsp.level`
  (levels by DFT, and a note sample by sample against a model in double), `dsp.envelope` (each ADSR segment to the
  sample, parameter changes that must not step), `dsp.voices` (allocation, stealing, note-off, All Notes Off, clicks),
  `dsp.render` (a phrase to WAV; block-size invariance), `plugin.process` (the processor as a host drives it: MIDI at
  offsets inside a block, host writes, NaN, bit-exact against the Engine; the MIDI that must never leave a note held:
  CC 120 and 123, a velocity-0 note-on, a 0-sample block, an overfull block, a bypass; a block larger than announced;
  the bypass edges, which must not click). Check against a model or a formula; a level alone misses a flipped
  polarity. `gui.dump` (the header with long names, a host's NaN, the envelope view) and `plugin.state` (NaN host
  text and state, calls before `prepareToPlay`) work from the table.
- `cmake --workflow --preset release-build`: Release with LTO in `build-release`. `scripts/install.sh` copies the AU
  and VST3 into `~/Library/Audio/Plug-Ins`; only the user (or the lead) runs it, never an agent.
- The presets build at `-j6` (the machine is shared). No network: JUCE, bgfx and shaderc come from `~/audio/.deps`
  (read-only, never write there) and FunkGui is cloned from `~/audio/libraries/FunkGui` at the pinned tag. FMinus is
  cloned from `~/audio/libraries/FMinus` at the pinned commit (`FP_FMINUS_SHA`; `-DFETCHCONTENT_SOURCE_DIR_FMINUS=`
  builds its working tree instead), and its LLVM, Bison and Flex are read from `~/audio/libraries/FMinus/.deps`, the
  installs FMinus's own builds made: never build them from here (FMinus's "system" LLVM mode, which `Deps.cmake` sets,
  never does).

## Layer and real-time rules

- `Source/dsp`, `Source/fsdsp` and `Source/fminus` include nothing from JUCE, FunkGui, `plugin/` or `editor/` (the
  test `lint.layers` checks; the DSP library has no JUCE include path). FMinus's internals and LLVM's headers appear
  only in `Source/fminus/*.cpp`.
- FMinus compiles (`FMinusProgram::compile`, `function`) off the audio thread only: they take milliseconds and
  allocate. A JIT'd function is called on the audio thread like any other, and the `FMinusProgram` outlives every call
  of it (README.md, "FMinus"). Mark every FMinus function the audio thread calls `[[clang::nonblocking]]`: FMinus then
  refuses it if it could reach the heap; a program's `calloc` and `free` run from `prepare()` and the end only.
- `Engine::setParams` and `Engine::process` run on the audio thread: no allocation, no locks, no I/O, no logging.
  Allocate in `prepare()`. Nothing steps: smooth every level parameter (the core's `Smoother4`, `SmoothRamp`,
  `smootherstep`), ramp a reused voice (a retrigger's level, a steal's fade-out before the new note: 2 ms,
  `fsdsp::kStealFadeMs`). Times may change slopes directly. The processor hands `setParams` finite values within the
  table's range (a host's NaN keeps the last good value).
- Notes play on their sample: `Engine::process` renders up to each event's offset, then applies it; ramps end on fixed
  samples (`render()` cuts there), so the output does not depend on the host's block size (`dsp.render` checks 64
  against 333 samples).
- `Engine::process` gets at most `prepare()`'s `maxBlockSize` samples per call (the processor cuts a larger host
  block into parts) and at most `kMaxEventsPerBlock` events, sometimes 0 samples: it applies the events then and
  renders nothing.
- No note-off is ever lost: `Engine::process` applies the events of a 0-sample block; the processor plays a block with
  more than `kMaxEventsPerBlock` events in parts, and while bypassed (`processBlockBypassed`) runs the Engine on the
  note-offs and all-notes-offs (no note-ons) and outputs silence, faded out and back in over 20 ms (smootherstep), so
  neither edge clicks: a note held through the bypass keeps sounding unheard and fades back in. `plugin.process`
  checks each case.
- The processor (`Source/plugin`, the same for both kinds) copes with the rest of what a host may do:
  a block before any `prepareToPlay` is silent, without calling the Engine; a host's NaN keeps the last good value
  (its host text is a dash); a state value that is not a number loads as the parameter's default (`plugin.state`).
- `Engine::tailSeconds()` is called by hosts from any thread: return a constant, or an atomic that `setParams` stores.
- Prefer the core's math on the audio path (`fsdsp::exp2/log2/tanh/sinPi/cosPi/tanPi`, `fsdsp::simd`, `fsdsp::phase`).
- Flags: `-ffp-contract=off`, never `-ffast-math`. Product constants come from the generated `Product.h`, never from
  `JucePlugin_*`. The editor never uses JUCE's parameter attachments: widgets write through FunkGui's
  `GestureController` to the ports (`funkgui::JuceParamPort`), which keeps host automation gestures right. A view
  that reads a port shows its last finite value (`ui::shownValue01`, as `EnvelopeView` does), never "nan".

## Recipes

- **A waveform**: `SineOscillator4::wave()`; then `dsp.level`'s MODEL and level rows (a sine's).
- **An envelope**: `adsr::Quad::tick()` (and `Bank` for note-on/off); then `dsp.envelope`.
- **A voice policy**: `VoiceScheduler::noteOn()`; then `dsp.voices`.
- **A parameter**: add an index and a row in `Source/dsp/Params.h` (id, name, label, unit, range, default, centre,
  control: dial, slider, choice or hidden); read `values[param::<name>]` in `Engine::setParams`. The processor, the
  host, the saved state and the panel follow. Never change an ID once sessions use it.
- **Another algorithm**: replace the per-sample loop in `Engine::renderSpan()` or the Engine's body (keep its
  interface), or add classes under `Source/dsp/` (the globs pick up new files: no CMake edit) and call them from it.
- **A widget or a view**: the panel lays out every table row itself. `EnvelopeView` is the example of a drawn view;
  for an interactive one add an `Item` in `Source/editor/Panel.cpp` (a FunkGui widget, or your own drawing on the
  `funkgui::Canvas` under a tag of its own: `kHeaderTextTag` and `kEnvelopeTag` are taken, the next free one is
  `ui::kEnvelopeTag + 1`) and place it in the constructor; then check `gui.dump`'s PNGs.
- **A probe**: a file `Tools/probes/<layer>/<name>.cpp` whose first line is `// PROBE name=<layer>.<name> timeout=<s>`
  and whose body is `PROBE(<layer>, <name>) { ...; return P.finish(); }`. Use spec rows (`P.near`, `P.le`, `P.ge`,
  `P.in`, `P.eq`) with tolerances that say what must hold; helpers are in `Tools/probes/common/Audio.h` and
  `Tools/probes/dsp/Rig.h` (a score of notes through the Engine).
- **Another plugin**: `scripts/plugin.py clone --name <Name> --code <Xxxx> [--type effect] [--starter]` (README.md).

## Git

Work on a branch; commit messages say what changed and why. Never commit build directories (`build-*/` is ignored).
Text about this repository's own history goes in `<!-- origin -->` blocks (README.md, CLAUDE.md): a clone has no
such history, and `plugin.py clone` leaves them out.
