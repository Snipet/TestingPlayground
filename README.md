# TestingPlaygroundSynth

A playground for audio research: an AU, VST3 and Standalone plugin for macOS on Apple silicon, built on JUCE 8.0.4,
with a GPU editor made with [FunkGui](https://github.com/Snipet/FunkGui) and the DSP core of FSynth (SIMD, fast math,
phase accumulators, smoothers, random numbers), vendored. Swap in your own algorithm; the probes measure what changed.

<!-- starter:intro -->
It is a polyphonic sine synth with an ADSR: sixteen voices, each a sine under an ADSR at its note's velocity, played
by MIDI on the sample each note arrives on, mono to both output channels. Five parameters: GAIN (-24 to +24 dB),
ATTACK (1 to 200 ms), DECAY (1 to 2,000 ms), SUSTAIN (0 to 100 %) and RELEASE (1 to 1,000 ms).

The editor is drawn from the parameter table: a picture of the envelope (ENVELOPE), a dial per parameter, the theme
and zoom cells. After a build, `build-dev/probe-out/gui.dump.png` is a picture of it (the `gui.dump` probe renders it
headless; `gui.dump.envelope.png` shows a decaying envelope with the pointer on DECAY).
<!-- /starter:intro -->

<!-- origin -->
It is the sine voice synth of the Visage version (tag `legacy-visage`), ported onto the DSP core. At the defaults
(SUSTAIN 100 %) it plays as that one did, but for the changes listed at the end of
[How the synth works](#how-the-synth-works). The Visage version stays in git:
`git show legacy-visage:Source/dsp/Voice.h` prints one of its sources, and
`git worktree add ../TestingPlaygroundSynth-legacy legacy-visage` checks the whole of it out beside this one.
<!-- /origin -->

## Build and run

Requirements: macOS 14 or later on Apple silicon, Xcode or its command-line tools, CMake 3.30 or later, Ninja,
Python 3. JUCE, bgfx and the shader compiler come from the machine cache `~/audio/.deps` (FCompressor's
`Scripts/deps.sh` fills it), FunkGui from `~/audio/libraries/FunkGui` at the pinned tag (`cmake/Deps.cmake`); a
machine without them fetches them from GitHub instead. FMinus comes from `~/audio/libraries/FMinus` at the pinned
commit, with the LLVM, Bison and Flex its own build left in its deps cache: build FMinus once first ("FMinus" below).

```sh
cmake --workflow --preset dev-verify     # configure, build everything, run every test (build-dev/)
open build-dev/TestingPlaygroundSynth_artefacts/RelWithDebInfo/Standalone/TestingPlaygroundSynth.app
```

In the Standalone, the Options button in the title bar picks the audio output and, for an instrument, the MIDI input
(a keyboard). An effect's Standalone starts with its input muted (JUCE's guard against feedback): unmute it there,
with headphones on. For a host:

```sh
cmake --workflow --preset release-build  # Release with LTO (build-release/)
scripts/install.sh                       # copy the AU and VST3 into ~/Library/Audio/Plug-Ins
```

Builds never install anything by themselves; `scripts/install.sh build-dev` installs a dev build instead. The plugin
code in `cmake/Product.cmake` (`scripts/plugin.py info` prints the whole identity) is this plugin's own, so it loads
in one host beside the user's other plugins.

## What is where

<!-- starter:where -->
```
cmake/Product.cmake          the identity: name, kind (instrument), version, codes, bundle id, C++ namespace
Source/dsp/                  the synth, plain C++20 without JUCE (namespace testingplaygroundsynth):
  Params.h                     the parameter table (GAIN, ATTACK, DECAY, SUSTAIN, RELEASE)
  SineOscillator.h             the waveform and the pitch: phase accumulators, four voices per SIMD vector
  Adsr.h                       the envelope, four voices per SIMD vector
  VoiceScheduler.h             which voice plays a note (the legacy policy)
  Engine.h/.cpp                the voices, the note events at their offsets, the ramps, the output gain
Source/fsdsp/core/           FSynth's DSP core, vendored at the tag in SEED.tsv (scripts/sync-dsp-core.sh)
Source/fminus/               the FMinus script: Script.h runs script.fm on the output (with FMinus's fminus/FMinus.h),
                             ScriptWatcher.h reloads it when the file changes
Source/plugin/               the JUCE AudioProcessor: parameters from the table, MIDI to note events, the Engine
Source/editor/               the FunkGui panel, built from the table; EnvelopeView (the ENVELOPE picture); Theme.h
Tools/probes/                tests that measure: dsp/ (the synth alone), plugin/ (the processor), gui/ (the panel),
                             fminus/ (FMinus's fminus::Program, the script)
scripts/                     plugin.py, test.sh, sync-dsp-core.sh, install.sh; starters/ (the template's starters)
CMakeLists.txt, cmake/       the build; every source list is a folder glob, so new files need no CMake edit
```
<!-- /starter:where -->

<!-- starter:algorithm -->
## How the synth works

- **Notes** reach `Engine::process()` as events with their sample offset in the block (`Source/dsp/Events.h`): it
  renders up to each event, applies it, and carries on, so every note starts and stops on its own sample. The
  processor turns MIDI note-on and note-off (a note-on at velocity 0 is a note-off) and the controllers All Sound Off
  and All Notes Off (both release every note) into these events; like the legacy synth it ignores the rest (pitch
  bend, sustain pedal, mod wheel). It never loses a note-off: not in a block of 0 samples, not in a block with more
  than 1,024 events (it plays such a block in parts), not while a host bypasses it (the voices carry on, no note
  starts). A host block larger than `prepare()`'s block size also reaches the Engine in parts.
- **Bypass**: when a host bypasses the plugin the output fades out over 20 ms (a smootherstep), and back in when the
  bypass ends, so neither edge clicks. Meanwhile the Engine runs on the note-offs only: a note let go during the bypass
  is not held afterwards, and a note held through it keeps sounding, unheard, and fades back in (its key is still
  down). Before the host's first `prepareToPlay` the output is silent and the Engine is not called; a parameter a
  host sets to NaN keeps its last good value (its host text is a dash, and the panel shows the last value).
- **Voices**: sixteen, in four SIMD vectors of four. A voice is a phase accumulator through a sine
  (`SineOscillator.h`), times the ADSR (`Adsr.h`), times its level, 0.25 x velocity (the legacy headroom: one note at
  full velocity peaks at -12 dBFS, four at 0 dBFS). The sum, times GAIN, goes to both channels.
- **Pitch**: 440 x 2^((note - 69) / 12) Hz, within 0.002 cents; the phase is a 32-bit integer that wraps, so a held
  note never drifts. A note that starts from silence starts at phase 0.
- **ADSR**, straight lines: the attack rises at 1 / ATTACK per second from wherever the voice is (from silence it lasts
  ATTACK); the decay falls from 1 to SUSTAIN in DECAY; the release falls from wherever the note-off finds it to 0 in
  RELEASE.
- **Voice policy** (the legacy one, `VoiceScheduler.h`): a note already sounding is played again on its own voice; else
  the first silent voice takes it; else the voice of the oldest note-on is stolen. A note-off releases the voice
  playing that note.
- **Nothing steps**: a stolen voice fades out over 2 ms (a smootherstep) and then starts its note, which therefore
  sounds 2 ms late; a note played again keeps its phase, and its level moves to the new velocity's over 2 ms; GAIN and
  SUSTAIN are smoothed per sample (20 ms); a change of ATTACK, DECAY or RELEASE changes a slope, never a level, and
  reaches the notes already playing.
- The output does not depend on how the host splits its blocks (the `dsp.render` probe compares 64 and 333 samples).

What changed from the legacy synth (at the defaults it otherwise plays the same notes at the same levels):

- **Clicks**: it clicked where a voice was reused: a stolen voice jumped to its new pitch and velocity mid-waveform, a
  retriggered one to its new velocity. Both now ramp over 2 ms (above).
- **Times**: a change of ATTACK or RELEASE reached only the notes started after it; now it reaches the notes playing.
- **GAIN** is smoothed over 20 ms and lands exactly on its value; the legacy one was smoothed too (a 10 ms one-pole),
  so it did not click. DECAY and SUSTAIN are new.
- **Release**: it now lasts RELEASE whatever the level at note-off. The legacy release fell at 1 / RELEASE per second,
  so a note let go at level L faded in L x RELEASE: the same for a note let go after its attack at SUSTAIN 100 %, but
  a note let go during its attack now fades for longer, up to 1 / L times (ATTACK 200 ms, let go after 50 ms: legacy
  0.25 x RELEASE, now RELEASE). `Source/dsp/Adsr.h` says how to restore the legacy rule.
- **Phase**: every note that starts from silence starts at phase 0. A legacy voice resumed at the phase its previous
  note stopped at (its oscillator was never reset); under an envelope that rises from 0 that is not audible, but the
  samples differ.

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
<!-- /starter:algorithm -->

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
through their ports and draws on the `funkgui::Canvas` (an area and a line); `Panel.cpp` places it and draws it when
the table has those four rows (ATTACK, DECAY, SUSTAIN, RELEASE). For an interactive view, add an `Item` in
`Source/editor/Panel.cpp` (FunkGui widgets: `Dial`, `RuleSlider`, `SegmentedSelector`, `LatchToggle`, ...). The panel
is GPU-free: the plugin runs it on Metal, the `gui.dump` probe on the CPU, so look at the PNGs after an editor change.
The header fits any product name: the panel widens for a long one, up to a limit, then leaves out the kind and
version, then cuts the name with an ellipsis.

## Test

```sh
scripts/test.sh                  # build the probes, run them; outputs in build-dev/probe-out/
scripts/test.sh -R dsp.render -V # one probe, every row printed
build-dev/TestingPlaygroundSynth_probes --list
build-dev/TestingPlaygroundSynth_probes dsp.render
```

<!-- starter:probes -->
| Probe | Checks |
| --- | --- |
| `dsp.pitch` | a held A4 at 44.1, 48 and 96 kHz and eleven notes from A0 to C8, from their zero crossings: within 0.5 cent (they measure under 0.001); no drift while held |
| `dsp.level` | one note at -12.04 dBFS, velocity and GAIN within 0.02 dB; a four-note chord, each note and the RMS; sixteen notes at once, each in the mix; a note sample by sample against a model in double |
| `dsp.envelope` | each ADSR segment to the sample (the note starts on its offset, the release on its note-off); GAIN, SUSTAIN and RELEASE changed mid-note never step |
| `dsp.voices` | sixteen notes on sixteen voices; the seventeenth steals the oldest after a 2 ms fade; a note played again keeps its voice; a note-off releases the right voice; the freed voice is reused; no click on a steal or a retrigger; All Notes Off during a steal's fade leaves nothing sounding |
| `dsp.render` | a short phrase (a melody over chords, ending with more notes than voices) to `dsp.render.wav`; finite, no clipping, block-size invariant |
| `plugin.process` | the processor as a host drives it: notes start on their sample offsets inside a block, at their velocity; GAIN written by the host reaches the DSP; a NaN from the host is ignored; the note-off leaves silence and no voice; the output is the Engine's bit for bit. No note is left held by All Notes Off or All Sound Off, a note-on at velocity 0, a note-off in a block of 0 samples, a block of more than 1,024 events, or a note-off while the host bypasses the plugin (whose output is then silent); a block larger than announced reaches the Engine in parts, with the same output; bypass and un-bypass with a note held fade over 20 ms with no step beyond the note's own (where a hard switch steps 17 times as far), then give silence and the never-bypassed output, bit for bit |
| `fminus.script` | the script on the audio thread (`Source/fminus/script.fm`): it loads; SCRIPT 0 leaves the output as it was, bit for bit; at 100 % it is finite and the same in blocks of 64 and 333; a swap settles within 50 ms with no step; a broken, a blocking and an incomplete script are refused while the running one plays on; an edited file is reloaded and its outcome logged |
| `fminus.program` | FMinus in this build, through its public API (`fminus/FMinus.h`): a block kernel and a stateful one compiled and called, bit for bit against C++, the same across block splits; `poly_lanes()`; a delay whose line the program allocates (aligned, silent), run by a `[[clang::nonblocking]]` kernel over two blocks and freed, and a nonblocking kernel that calls `malloc` refused; a program that does not compile and a function that does not exist give FMinus's messages; compiling takes under a second (about 10 ms) |
| `gui.dump` | the panel headless to `gui.dump.png`, `gui.dump.paper.png` and `gui.dump.envelope.png`; settles, every glyph, a11y; the header clear of the THEME cells with this name and with names of 24, 48 and 64 letters (`gui.dump.longname.png`); a drag writes the parameter; a host's NaN shows the last value; the envelope follows the parameters, keeps its shape through a host's NaN and highlights the segment under the pointer |
| `plugin.state` | the host's parameters equal the table; the state round-trips; a NaN's host text is a dash; a state value that is not a number loads the default; processBlock and processBlockBypassed before prepareToPlay are silent; processBlock gives finite output |
<!-- /starter:probes -->

`dsp.*` and `plugin.process` test the algorithm: change them with it. `gui.dump` and `plugin.state` work from the
parameter table, whatever the algorithm. `cmake --workflow --preset dev-verify` runs them all with the build, plus
`lint.layers` (nothing in `Source/dsp` or `Source/fsdsp` includes JUCE, FunkGui, `plugin/` or `editor/`).

A probe is one file in `Tools/probes/<layer>/` whose first line registers it (`// PROBE name=dsp.mytest timeout=60`)
and whose body uses spec rows (`P.near`, `P.le`, `P.ge`, `P.in`, `P.eq`): each states what must hold, with its
tolerance. `Tools/probes/common/Audio.h` has the helpers (sines, levels, a DFT bin, frequency, WAV output);
`Tools/probes/dsp/Rig.h` runs the Engine as the processor does.

## FMinus

[FMinus](https://github.com/Snipet/FMinus) (`~/audio/libraries/FMinus`), the JIT compiler for DSP, is linked in. Its
public API, `fminus/FMinus.h` (`fminus::Program`), compiles FMinus source (C with vectors: its README has the language)
while the plugin runs and hands back its functions as C function pointers, so a piece of an algorithm can be written in
FMinus and compiled while the plugin runs, from a string or from a file it reads. Any file of `Source/dsp/` may use it:

```cpp
#include "fminus/FMinus.h"

// In the Engine: the program, and the function the audio thread calls.
std::unique_ptr<fminus::Program> program_;
void (*gain_)(const float* in, float* out, int n, float g) = nullptr;

// Engine::prepare(), off the audio thread: compiling takes milliseconds and allocates.
std::string errors;
program_ = fminus::Program::compile(R"(
    void gain(const float* restrict in, float* restrict out, int n, float g)
    {
        for (int i = 0; i < n; ++i)
            out[i] = in[i] * g;
    }
)", errors);
gain_ = program_ ? program_->function<void(const float*, float*, int, float)>("gain", errors) : nullptr;
// When gain_ is null, errors says why: FMinus's messages, as clang words them, with line and column.

// Engine::process(), over the voices' sum: one call a block, and the loop is FMinus's.
if (gain_)
    gain_(in, out, n, g);
```

- **Never compile on the audio thread.** Compile in `prepare()`, or on another thread and publish the new pointer
  through an `std::atomic`; then keep the old `fminus::Program` until the audio thread has finished the block that may
  still run its code. The pointers die with their program.
- **The pointer's type must be the function's C type**: `float` is `float`, `int` is `int`, a pointer a pointer, a
  `float4` clang's `ext_vector_type(4)` (`fminus/FMinus.h` has the table). Nothing checks it.
- **The rest of the API**: `fminus::Program::Options` (the optimisation level, a `poly float`'s lanes, the name the
  errors give), `polyLanes()`, and `nonBlocking(name)` and `nonAllocating(name)`, which say what FMinus has checked
  (`Script::load` refuses a `process` that is not nonblocking). FMinus's README, "Using FMinus as a library", has the
  rest.
- **State lives on the heap, allocated in `prepare()`**: the Engine's own memory, passed by pointer, or the program's.
  A program allocates with `calloc` (64-byte aligned, zeroed) in a function the Engine calls from `prepare()`, and
  frees with `free` in one it calls at the end; the function the audio thread calls is marked
  `[[clang::nonblocking]]`, and FMinus refuses to compile it if it could reach `malloc`, `calloc` or `free`, however
  far down (FMinus's `examples/delay/`, and its docs/extending.md, recipe 11). Either way, the FMinus function reads its
  state at the start of a call and writes it back at the end, so the output does not depend on how the blocks are
  split.
- **Which FMinus**: the commit `FP_FMINUS_SHA` in `cmake/Deps.cmake`, from `~/audio/libraries/FMinus`. To try a change
  to FMinus before committing it, configure with `-DFETCHCONTENT_SOURCE_DIR_FMINUS=$HOME/audio/libraries/FMinus`:
  the working tree is built as it is. LLVM, Bison and Flex are the ones FMinus's own build put in
  `~/audio/libraries/FMinus/.deps`, read and never rebuilt here: build FMinus once
  (`cmake --workflow --preset dev` there) before configuring this plugin. A Release build here takes LLVM without
  assertions when FMinus's release preset has made it, and the dev flavor otherwise.
- **The cost**: LLVM is linked into each binary, about 80 MB more in a Release build, and stays private to it: each
  bundle exports only its entry points (`lint.exports` checks), so two plugins with LLVM in one host do not share it.
  A DAW's hardened runtime must allow the JIT's executable memory (`com.apple.security.cs.allow-jit` or
  `allow-unsigned-executable-memory`; Ableton Live 12 has the second); the Standalone and the probes always do.
  FMinus's README, "Notes for running inside a plugin", has the rest.

### The script, live

The Engine runs an FMinus script on its output, after the voices' sum and GAIN: `Source/fminus/script.fm`, a wavefolder
(the sines fold over themselves, brighter the further SCRIPT goes), followed by a 9 kHz low-pass that tames the folds'
aliasing. The SCRIPT dial says how much of it you hear: at 0 %, the default, the script does not run and the synth
sounds as it always did; at 100 % a sine is folded over four times.

Edit `script.fm` while the plugin runs, in the Standalone or in a DAW, and save: half a second later every instance
compiles the new script and fades over to it, 20 ms out and 20 ms in. `script.fm.log`, beside it, says how it went:
`loaded, compiled in 9 ms`, or FMinus's errors with their lines and columns, while the old script plays on. The plugin
also keeps the script as it was when it was built (`build-*/generated/ScriptSource.h`) and starts with that, so an
installed plugin plays its script where the file is not.

A script is three functions (`Source/fminus/Script.h` has the details):

```c
float* create(float rate);      // off the audio thread: the script's state (calloc), or null
void process(float* restrict state, float* restrict left, float* restrict right, const float* restrict amount,
             int n) [[clang::nonblocking]];    // the audio thread: a block of both channels, in place
void destroy(float* state);     // off the audio thread: frees the state
```

`amount` is SCRIPT, per sample, 0 to 1, smoothed. Write the script so that 0 leaves the sound as it is: the Engine
stops calling it there, and a script that changed the sound at 0 would step when it stopped. `process` must be marked
`[[clang::nonblocking]]`: FMinus then refuses to compile it if it could reach `malloc`, `calloc` or `free`, and the
plugin refuses a `process` without the mark. The state is the script's own, from `create`: read it at the start of a
call and write it back at the end, and the output does not depend on the host's block size.

The probe `fminus.script` checks the script in this build, whatever the file holds when you build: it loads; at SCRIPT 0
the output is the synth's alone, bit for bit; at 100 % it is finite and the same in blocks of 64 and 333; a swap settles
within 50 ms without a step; a broken script, one whose `process` is not nonblocking, and one without `destroy` are
refused while the running one plays on; and an edited file is picked up and its outcome logged. It also writes what the
script does to `build-dev/probe-out/fminus.script.wav` (a second at SCRIPT 100 %), beside `fminus.script.dry.wav` (the
same second without it): `scripts/test.sh -R fminus.script` renders them, to listen to without a DAW.

The probe `fminus.program` checks it in this build: a block kernel and a stateful one, bit for bit against C++ and
across block splits, `poly_lanes()`, a delay line on the heap and a nonblocking kernel refused for calling `malloc`,
the errors, and the compile time (about 10 ms).

## Make a new plugin

```sh
scripts/plugin.py clone --name MyPlugin --code Mypl                    # this project, as a new plugin (../MyPlugin)
scripts/plugin.py clone --name MyPlugin --code Mypl --starter          # the template's starter of this kind instead
scripts/plugin.py clone --name MyDelay --type effect --code Mydl       # an effect, from the template's starter
```

The clone gets its own name, plugin code, bundle id and C++ namespace (asked for, or derived from the name), keeps the
manufacturer code, and builds without edits. It refuses a plugin code another of your plugins already uses (any
project in `~/audio/plugins`, or beside the source or the destination): two plugins with one code hide each other in a
host. `--type` switches the kind, and `--starter` restarts from the template's starter of the same kind
(`scripts/starters/`): the algorithm, its probes, CLAUDE.md and this README's kind-specific blocks (the ones between
`<!-- starter:... -->` markers). A clone has no git history, so it leaves out the `<!-- origin -->` blocks, which
point into this project's. `plugin.py rename` renames this project in place; `plugin.py starter-diff` shows how this
project differs from its kind's starter.

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
