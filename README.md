# TestingPlaygroundSynth

An instrument (AU, VST3 and Standalone, macOS on Apple silicon) whose voices you write while it plays: its editor is a
text field holding an [FMinus](https://github.com/Snipet/FMinus) program, the plugin compiles it when you press RUN
(or Cmd-Return), and the next block plays it; the keys you hold start again on the new program while the old one's
voices fade out. The program's parameters become the dials under the text. Built on JUCE 8.0.4, with a GPU editor made
with [FunkGui](https://github.com/Snipet/FunkGui) (its `TextEditor`) and FMinus, the JIT compiler for DSP, linked in.

<!-- origin -->
It began as the sine voice synth of the Visage version (git tag `legacy-visage`), ported onto FSynth's DSP core and
made from FunkPluginTemplate; that synth, and the file-watching script that ran after it, are in the history before
the branch `fminus-editor`. TestingPlayground (`../TestingPlayground`) is the effect with the same editor.
<!-- /origin -->

After a build, `build-dev/probe-out/gui.dump.png` is a picture of the editor.

## Writing a program

FMinus is C with DSP's additions (vectors, modules, parameters); its README has the language, and its
`docs/programs.md` and `docs/extending.md` the details. The plugin plays a module named `Voice`, one instance a note,
sixteen at once:

```c
module Voice
{
    param float release [[fminus::range(5, 5000)]] [[fminus::default(300)]];   // ms: a dial
    float rate, phase, step, env;                                              // state: the module's fields
    bool held;

    void init(float sampleRate)                // optional: before the first note, and when the sample rate changes
    {
        rate = sampleRate;
    }

    void start(float note, float velocity)     // a note-on: 69 is A, 440 Hz; velocity 0 to 1
    {
        step = 440 * expf(0.0577622650f * (note - 69)) / rate;
        env = velocity;
        held = true;
    }

    void stop()                                // optional: the note-off
    {
        held = false;
    }

    bool process(float* restrict left, float* restrict right, int n)   // the voice's next n samples
    {
        const float down = expf(-6907.7553f / (release[0] * rate));
        for (int i = 0; i < n; ++i)
        {
            if (!held)
                env *= down;
            const float out = env * sinf(6.28318531f * phase);
            left[i] += out;
            right[i] += out;
            phase += step;
            if (phase >= 1)
                phase -= 1;
        }
        return held || env > 0.0001f;          // false: silent for good, and the voice is free
    }

    void end() {}                              // optional: before the program goes (free what init allocated)
};
```

- **start** begins a note on a voice, which may have played another before: set what the note needs (a voice's
  fields keep their values between notes, and start as 0 when the program does). **stop** releases it. **process**
  adds the voice's next n samples into both channels (they arrive at 0, so writing works too) and returns false once
  the voice is silent for good, which frees it. Keep the state per sample, and the output does not depend on the
  host's block size: the plugin calls the voices in parts that end on multiples of 16 samples, and frees a finished
  voice only there.
- **Voices.** Sixteen sound at once. A seventeenth note steals the oldest released voice (else the oldest), fading it
  out over 2 ms while the new note starts on another instance. A key pressed again while its note still sounds stops
  that voice first. The voices are summed, a mono output gets their two channels' average, and sustain pedals and
  pitch bend are not passed on (yet).
- **Dials.** The module's first eight parameters, in the order they are declared, are the dials, and the host's
  parameters P1 to P8 (a host shows "1 release", and the value in the program's units); every voice reads the same
  values. A `float` glides when its dial turns: it is read sample by sample, `release[i]`, as a stream of 20 ms ramps,
  and held still otherwise (`release[0]` reads it once a call). An `int` or a `bool` is read once a call.
  `fminus::range` sets the dial's span (a float without one spans 0 to 1, an int 0 to 10) and `fminus::default` where
  it starts and where a double-click returns it. A run that brings a parameter the slot did not have moves that dial
  to its default; one with the same name keeps it.
- **The audio thread.** start, stop and process may not allocate: FMinus refuses a program whose methods could reach
  `calloc`, `malloc` or `free` from them, and names the line. Allocate in init (each voice's own: the Pluck example's
  delay line) and free in end, both off the audio thread; or use a fixed array, a field like any other.
- **Modules inside modules.** Voice may hold others (`Svf filter;`, `Osc osc[3];`): their methods are its helpers, and
  their parameters start at their defaults (they have no dials).
- **Its types are checked.** The plugin calls each method through a line of its own with the type it calls it by, so
  a `process` that returns nothing, or takes other parameters, is an error ("Voice::process: the plugin calls it as
  ..."), never a crash.
- **What the plugin guards, and what it cannot.** Every sample that leaves the plugin is finite and within +-4
  (+12 dBFS): a voice whose output is not finite is dropped, a louder sample is clipped, and the toolbar says so. But
  the program is native code: a write through a pointer past its memory, or a loop that never ends, takes the host
  with it. Try a new program in the Standalone.
- **Subnormals** are flushed to zero (the plugin compiles with FMinus's `Denormals::Flush`, the mode the audio thread
  runs in), and the math builtins (`sinf`, `cosf`, `expf`, `tanhf`, `logf`; also `fabsf`, `sqrtf`, `floorf`, `fminf`,
  `fmaxf`) are FMinus's default math mode's: one result for a float wherever it is computed.

The EXAMPLES menu holds four to start from (`Source/fminus/examples/`): Sine (an attack and a release; the program a
new instance plays), Saw and filter (a polyBLEP sawtooth through a resonant low-pass, an ADSR on both: eight dials),
FM bell (two operators) and Pluck (Karplus-Strong, each voice's delay line from calloc in init). To add one, add a
file there whose first line is `// <Title>: what it is`.

## The editor

- **RUN** compiles the text and plays it once it compiles; **Cmd-Return** does the same from the keyboard. The
  sounding voices fade out over 20 ms, and the keys still held start again on the new program. Compiling takes some
  tens of milliseconds, off the audio thread and off the editor's.
- **EXAMPLES** replaces the text with an example (Cmd-Z gives yours back) and runs it.
- **The status** says whether the program plays, whether the text has changed since it was run, and how many errors
  the last run had; a program that does not compile leaves the last one that did playing. At the right: the audio
  thread's load (the busiest recent block's share of its time) and the output guard's alerts.
- **The text** is FunkGui's TextEditor: FMinus's words highlighted, line numbers, undo (Cmd-Z, Cmd-Shift-Z), the
  clipboard, word and line moves (Alt and Cmd with the arrows), Tab to indent (four spaces), Escape to leave the text
  (the panel's keys then move the focus over the dials). It has the caret when the editor opens.
- **Problems.** The last run's messages are listed under the text, with their line and column; a click on one puts the
  caret there, and each line with an error has a mark in the gutter.
- **The dials** are the program's parameters (above); drag, scroll, or use the keys once Tab has focused one.
- **The session** keeps the program that plays, the text as you left it (run or not) and the dials. A session whose
  program no longer compiles (an older FMinus, say) leaves the first example playing, its text in the editor.
- THEME (header) and ZOOM (footer) are machine-wide preferences.

## Build and run

Requirements: macOS 14 or later on Apple silicon, Xcode or its command-line tools, CMake 3.30 or later, Ninja,
Python 3. JUCE, bgfx and the shader compiler come from the machine cache `~/audio/.deps` (FCompressor's
`Scripts/deps.sh` fills it), FunkGui from `~/audio/libraries/FunkGui` at the pinned commit (`cmake/Deps.cmake`); a
machine without them fetches them from GitHub instead. FMinus comes from `~/audio/libraries/FMinus` at the pinned
commit, with the LLVM, Bison and Flex its own build left in its deps cache: build FMinus once first ("FMinus" below).

```sh
cmake --workflow --preset dev-verify     # configure, build everything, run every test (build-dev/)
open build-dev/TestingPlaygroundSynth_artefacts/RelWithDebInfo/Standalone/TestingPlaygroundSynth.app
```

In the Standalone, the Options button in the title bar picks the audio output and the MIDI input. For a host:

```sh
cmake --workflow --preset release-build  # Release with LTO (build-release/)
scripts/install.sh                       # copy the AU and VST3 into ~/Library/Audio/Plug-Ins
```

Builds never install anything by themselves; `scripts/install.sh build-dev` installs a dev build instead. The plugin
code in `cmake/Product.cmake` (`scripts/plugin.py info` prints the whole identity) is this plugin's own, so it loads
in one host beside your other plugins. A host that keeps keys for itself (some do for the space bar or the arrows, or
play notes from the computer keyboard) may need its "keyboard to plugin" option on for typing in the editor.

## Test

```sh
scripts/test.sh                       # build the probes, run them; outputs in build-dev/probe-out/
scripts/test.sh -R fminus.contract -V # one probe, every row printed
build-dev/TestingPlaygroundSynth_probes --list
```

| Probe | Checks |
| --- | --- |
| `fminus.examples` | every example compiles with no message, its dials named, with ranges and defaults inside them; plays a chord for a second and releases it, finite, audible and with no alert; the same bits in blocks of 64 and 333; its voices end within eight seconds |
| `fminus.contract` | what a program needs, and what the plugin says when it does not fit: no module `Voice`, no `start`, a `process` of another type (refused, never called), one that could allocate, a syntax error with its line; eight dials and a note for the ninth parameter; a held module's parameters at their defaults; init gets the rate |
| `dsp.swap` | the Engine: a held note's voice fades out when another program arrives, which starts the key again; seventeen notes steal the oldest; a voice that returns false is free; a real dial glides over 20 ms sample by sample, an integer one steps at the block; a NaN, an infinity and 10 leave as 0, 0 and 4, with their alerts |
| `plugin.process` | the processor as a host drives it: P1 to P8 named and worded as the program's, host writes reach the program (within 0.05 dB), a host's NaN ignored (its text a dash), a block of 0 samples (with a note-on in it) and a block larger than announced change nothing, the bypass is silent and its edges do not click, a block before prepareToPlay is silent; a note-on at velocity 0 and All Notes Off release |
| `plugin.state` | the session round-trips (the program, the draft, the dials, the output bit for bit); a run moves a renamed parameter's dial to its default and keeps a kept one's; a session whose program does not compile leaves the last one playing; one without a program plays the first example; a NaN dial loads as 0.5 |
| `gui.dump` | the panel headless to `gui.dump.png`, `.paper.png`, `.error.png` and `.edited.png`: settles, every glyph, the draft in the editor and a dial for each parameter; typing, Cmd-Return, a syntax error (status, problem line, gutter mark, the click that jumps to it), the EXAMPLES menu and its undo, a dial's drag and double-click, Escape and Tab |
| `lint.layers`, `lint.exports` | Source/dsp, Source/fsdsp and Source/fminus without JUCE or FunkGui, FMinus through its public header only; each bundle exports its entry points and nothing else |

A probe is one file in `Tools/probes/<layer>/` whose first line registers it (`// PROBE name=dsp.mytest timeout=60`)
and whose body uses spec rows (`P.near`, `P.le`, `P.ge`, `P.in`, `P.eq`): each states what must hold, with its
tolerance. `Tools/probes/common/Audio.h` has the signal helpers, and `Tools/probes/common/Rig.h` runs a program in an
Engine as the processor does, notes at absolute samples.

## FMinus

[FMinus](https://github.com/Snipet/FMinus) (`~/audio/libraries/FMinus`) comes in through its public API,
`fminus/FMinus.h` (`fminus::Program`): `Source/fminus/Patch.cpp` compiles a program twice (once to read its module
and parameters, once with each float parameter bound as a stream and the plugin's calls appended), at `-O2`, flushing
subnormals, and makes 32 instances of Voice.

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

## What is where

```
Source/fminus/           Patch: a program compiled for the plugin (the contract, the calls, the instances), JUCE-free;
                         examples/*.fm, the EXAMPLES (compiled in by CMakeLists.txt as generated/Examples.h)
Source/dsp/              the audio side, plain C++20 without JUCE: Engine (the voices, programs swapped without a
                         click, the dials glided into streams, the output guard), Compiler (compiles off the audio
                         thread), Controls, Events
Source/fsdsp/core/       FSynth's DSP core, vendored at the tag in SEED.tsv (scripts/sync-dsp-core.sh)
Source/plugin/           the JUCE AudioProcessor: MIDI to events, P1 to P8, the program and the draft in the session,
                         the bypass; EditorPort.h, what the editor reaches besides the parameters
Source/editor/           the FunkGui panel (Panel.cpp: the toolbar, the TextEditor, the problems, the dials), Theme.h;
                         gpu/ starts it in the plugin
Tools/probes/            tests that measure: fminus/, dsp/, plugin/, gui/ (the panel, headless)
scripts/                 plugin.py, test.sh, sync-dsp-core.sh, install.sh
cmake/Product.cmake      the identity: name, kind (an instrument), manufacturer and plugin codes, bundle id, namespace
```

## Make a new plugin

Start new plugins from FunkPluginTemplate (`../FunkPluginTemplate/scripts/plugin.py clone --name MyPlugin`, effect or
instrument). This project's own `scripts/plugin.py clone --name MySynthLab --code Msyl` copies the playground into a
new project with its own identity; `--type effect`, `--starter` and `starter-diff` refuse here, because the starters
live in the template. The clone refuses a plugin code another of your plugins already uses (any project in
`~/audio/plugins`, or beside the source or the destination): two plugins with one code hide each other in a host.
`scripts/plugin.py rename` renames this project in place.

## Update the DSP core

```sh
scripts/sync-dsp-core.sh <fsynth-tag>     # e.g. fsyn-s3 (the seed, SEED.tsv) or a later fsyn-sN
```

It takes `Source/fsdsp/core` from FSynth at that tag (`git tag` in FSynth lists them) and records it in SEED.tsv.
It refuses to overwrite a core file you edited (`--force` overwrites). Build and test after a sync.

## Theme, zoom, diagnostics

The THEME cells (header) and the ZOOM cells (footer) are machine-wide preferences, stored by FunkGui in
`~/Library/Application Support/TestingPlaygroundSynth/`. Colours are tokens: `Source/editor/Theme.h` shows how to give
this plugin its own accent; errors use the theme's `signal` colour. `TSYN_UI_THEME=1` opens the Standalone on the
light theme without saving it, `TSYN_GPU_LOG=1` logs the GPU surface (the prefix is the product's,
`cmake/Product.cmake`).
