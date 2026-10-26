# TestingPlaygroundSynth

An instrument (AU, VST3 and Standalone, macOS on Apple silicon) whose voices you write while it plays: its editor is a
text field holding an [FMinus](https://github.com/Snipet/FMinus) program, the plugin compiles it when you press RUN
(or Cmd-Return), and the next block plays it; the keys you hold start again on the new program while the old one's
voices fade out. The program's parameters become the dials under the text. A chat with Claude at the right of the
editor writes and changes the program for you: describe a sound and it writes the FMinus voice, runs it, fixes what
does not compile and leaves the dials at good settings, so a sentence does what a preset did. Built on JUCE 8.0.4, with
a GPU editor made with [FunkGui](https://github.com/Snipet/FunkGui) (its `TextEditor`) and FMinus, the JIT compiler
for DSP, linked in.

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
- **Dials.** The module's first 32 parameters, in the order they are declared, are the dials, and the host's
  parameters P1 to P32 (a host shows "1 release", and the value in the program's units); every voice reads the same
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
- **Shared data.** A module named `Shared` is made once for the program, not once a voice: its init runs before the
  voices' (with the rate), its end after theirs, and every field of Voice of type `Shared*` points at it. So the
  voices read one wavetable, lookup table or sample that init built once, where a voice's own init would build it for
  every voice instance. The voices read it on the audio thread and never free it.

  ```c
  module Shared
  {
      Wavetable table;                                       // wavetable.fm
      void init(float rate) { table.allocate(1); table.shape(0, 2); }
      void end() { table.release(); }
  };

  module Voice
  {
      Shared* shared;                                        // the plugin sets it before init
      WaveOsc osc;
      void init(float rate) { osc.table = &shared->table; }
      ...
  };
  ```
- **The FFT.** FMinus has one, real and complex: a plan from `fft_create(n)` in init (n a multiple of 32 made of 2s,
  3s and 5s: 256, 1024, 2048), `fft_forward(plan, in, bins)` and `fft_inverse(plan, bins, out)` anywhere, the audio
  thread too (n floats to n / 2 + 1 bins and back, unscaled: the round trip is n times the input), `fft_destroy(plan)`
  in end. `wavetable.fm` builds band-limited wavetables with it.
- **Several files.** The editor has a tab for each file, the program's first (`program.fm`). The program includes
  another by its name, `#include "oscillators.fm"`, as C does: the file is read where the line is (a module there is a
  type after it), and once only, as if every file began with `#pragma once`, so two files may both include a third.
  Its errors name it ("oscillators.fm:3:12: error: ..."). FMinus has no other directive: no macros, no `#if`.
- **The library.** Ready-made building blocks, tested by `fminus.library`: filters (`svf.fm`, `ladder.fm`,
  `onepole.fm`, `biquad.fm`), delays and reverb (`delay.fm`, `reverb.fm`), sources (`osc.fm`, `wavetable.fm`,
  `noise.fm`), modulation (`adsr.fm`, `lfo.fm`, `follower.fm`), shaping and utilities (`shaper.fm`, `util.fm`).
  LIBRARY (the editor) or Claude adds one as a tab of the program's own; `#include` it and use its modules (each
  file's header says how). It is then the program's file like any other, saved with it, and yours to change. They
  live in `Source/fminus/library/<n>-<category>/`.
- **The panel.** `#pragma panel` lines lay the dials out as an analog synth's panel is printed, in titled sections,
  framed, in rows; they choose how each dial looks and add displays that show what the program does as it plays
  (FMinus ignores every `#pragma`; the plugin reads these from the program and what it includes):

  ```c
  #pragma panel section "OSCILLATORS" shape detune      // a section: its title, then its dials and displays in order
  #pragma panel section "FILTER" cutoff resonance amount
  #pragma panel row                                     // the sections after it start a new row
  #pragma panel section "ENVELOPE" attack decay sustain release
  #pragma panel section "OUTPUT" level out              // out: the VU below
  #pragma panel label amount "ENV AMOUNT"               // a dial's label (else its name)
  #pragma panel small detune                            // a small knob: its label under it, no value
  #pragma panel style cutoff vintage                    // knob (the default), vintage, fader, switch or buttons
  #pragma panel style attack decay sustain release fader
  #pragma panel style shape buttons
  #pragma panel options shape "SAW" "SQUARE" "TRI"      // an int's values' names, from its range's low end
  #pragma panel color "FILTER" orange                   // a section's colour, or a dial's or a display's
  #pragma panel vu out "OUTPUT"                         // a display: meter, vu, led, number or scope
  ```

  - **Dial styles**: `knob`; `vintage`, a black knob over a printed scale; `fader`, a slider; `switch`, a toggle (a
    bool, or an int of two values); `buttons`, a lit button a value (an int or a bool, up to eight). A `style` line
    names one dial or several, the style last. `options` names an int's values (a bool's: off, on): its buttons show
    them, and its knob and the host say them.
  - **Displays** show the plugin's output, `out` (left and right, in dBFS; an instrument has no `in`), or a float,
    int or bool field of the voice that process writes (`env = level;`), across a range given after it (0 to 1
    unless it says: `#pragma panel meter cutoffhz 20 20000 "HZ"`), from the newest voice that sounds. `meter`: LED
    bars, the highest held a second (the output's peaks); `vu`: an analog VU meter's needle (0 VU at -18 dBFS RMS);
    `led`: a light, a field's value its brightness (of the output: a clip light); `number`: a readout of the field
    (of the output: its peak in dBFS); `scope`: an oscilloscope (of a field: its trace over the last 2.5 seconds). A
    section names a display by its source, or by the name `as` gives it, which a second display of one source needs:
    `#pragma panel scope out as wave "WAVE"`. Eight displays at most. The colours: red, orange, yellow, green, teal,
    blue, violet, pink, cream, grey.

  A row too wide for the window wraps, its sections shared out evenly; shorter rows widen toward the widest; when
  the sections need more height than the window gives, every knob is drawn small. Dials and displays no section
  names come last, untitled. A line the plugin cannot use (a name that is no dial, a dial in two sections, a field
  that is not there, a style the dial does not suit) is a note on its line, and the rest stands
  (`Source/fminus/PanelSpec.h`).
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
new instance plays), Saw and filter (a polyBLEP sawtooth through a resonant low-pass, an ADSR on both: eight dials,
the envelope's on faders, a light that follows it and a VU), FM bell (two operators; a meter of its brightness and a
scope) and Pluck (Karplus-Strong, each voice's delay line from calloc in init). To add one, add a
file there whose first line is `// <Title>: what it is`.

## The editor

- **RUN** compiles the text and plays it once it compiles; **Cmd-Return** does the same from the keyboard. The
  sounding voices fade out over 20 ms, and the keys still held start again on the new program. Compiling takes some
  tens of milliseconds, off the audio thread and off the editor's.
- **EXAMPLES** replaces the program's text with an example (Cmd-Z gives yours back) and runs it; the other files
  stay.
- **LIBRARY** lists the library's files by category, a tick on those the program has. One it lacks becomes a tab,
  shown, and the program `#include`s it (a line after its description; Cmd-Z in the program takes it back); one it
  has is shown. With twelve tabs, the rest are greyed.
- **RANDOM** writes a new voice for inspiration, from the library's building blocks: one or two oscillators (a
  band-limited morph, a wavetable built once in `Shared`, or two operators of FM), noise, a filter its envelope
  opens, an envelope of a character (a pluck, keys, a pad, a bass, brass, a bell), an LFO and a drive, with random
  settings, a panel and a name. It replaces the program's text (Cmd-Z gives yours back), adds the library files it
  needs as tabs, and runs it; each click makes another. Its first line names it and its seed (`random 4821`).
- **PRESETS** keeps programs on this Mac: the editor's files (every tab, and the one shown) and the dials' values,
  under a name, in `~/Library/Audio/Presets/Funk/TestingPlaygroundSynth/` (a JSON file a preset, `<name>.fmpreset`:
  copy, rename or share them in the Finder). The browser lists them with when each was saved: a click chooses one, a
  double-click or **Load** loads it (its files become the tabs and run, and its dials are set by name), **Save** saves
  the editor's program under the name in the field, **Delete** deletes the chosen one. Replacing a preset, loading
  over changes no preset holds, and deleting each ask first (a second click). The header names the preset loaded or
  saved last, and says EDITED once the files change.
- **The files' tabs**, over the text: a click shows a file; a double-click edits its name in place (Return keeps it,
  Escape cancels; letters, digits, `_`, `-` and `.`, and no two the same); **+** adds a file (up to twelve; the tabs
  narrow to fit), its name ready to type; a tab's **x** closes its file, asking first when it has text. The program's tab cannot be closed. A
  tab whose file the last run's errors name has a mark before its name. Each file keeps its own caret, scroll and undo.
- **The status** says whether the program plays, whether the text has changed since it was run, and how many errors
  the last run had; a program that does not compile leaves the last one that did playing. At the right: the audio
  thread's load (the busiest recent block's share of its time) and the output guard's alerts.
- **The text** of the file shown is FunkGui's TextEditor: FMinus's words highlighted, line numbers, undo (Cmd-Z,
  Cmd-Shift-Z), the clipboard, word and line moves (Alt and Cmd with the arrows), Tab to indent (four spaces), Escape to
  leave the text (the panel's keys then move the focus over the dials). It has the caret when the editor opens.
- **Problems.** The last run's messages are listed under the text, with their file, line and column; a click on one
  shows its file and puts the caret there, and each line with an error has a mark in the gutter.
- **The dials** are the program's parameters (above): in the sections its `#pragma panel` lines ask for (above),
  in their styles and beside its displays, or in order, in as few rows as the width holds (sixteen in two rows of
  eight at the default size, in one row on a wide window, 32 in three; every knob small when the window is too short
  for them); drag, scroll, or use the keys once Tab has focused one. A switch or a button takes a click (Return or
  Space steps it). The displays move with the sound, as hardware does.
- **The session** keeps the program that plays (all its files), the files as you left them (run or not), the tab
  shown, the dials and the preset's name. A session whose program no longer compiles (an older FMinus, say) leaves the
  first example playing, its files in the editor. A session saved before files loads as a program of one file.
- **The window** resizes in both directions, from 1000 x 600 (logical px, before ZOOM) up: the editor and the chat's
  conversation take the height, the chat about a third of the width. The session keeps the size.
- THEME (header) and ZOOM (footer) are machine-wide preferences.

## The chat

The rounded card at the right is a conversation with Claude, through the [Claude API](https://docs.claude.com) with
your own key. Claude sees the plugin's files, whether they play, the last compile's messages and the dials (sent with
a message when they changed; a file it has already seen goes as one line, not again in full), and it can act on
them: read, write, edit, rename and delete the files (the editor's tabs follow, and Cmd-Z in a file undoes its
change), add the library's building blocks, run the program (RUN; the compile's messages come back to it), set the
dials, measure what the program puts out, and read the CPU load and the output guard's alerts. Ask it for a sound ("a warm detuned pad", "a plucked
string"), a change ("a slower attack", "more resonance"), an explanation ("what does this voice do?"), or help with
an error or a click. It knows FMinus, the plugin's contract, the library (each file's header) and how to build a
large program from it, and it lays the dials out in a panel (its instructions are in `Source/plugin/ChatPrompt.cpp`).

- **The conversation**: your messages in bubbles at the right; Claude's answers in light markdown (lists, headings,
  **bold**, `code`, and code blocks with a **Copy** of their own); a **Copy** under each finished answer (it shows
  under the pointer); Claude's thinking as a row that opens and closes; what it did (a file written, a run, a dial
  set, a sound measured) as a card of rows; errors in a tinted card. While Claude works, pulsing dots say what it is
  doing ("Writing osc.fm (12 KB)"). The wheel scrolls; a round button brings you back to the end. An empty chat
  offers suggestions: a click puts one in the message box, and nothing is sent until you send it.
- **The message box** grows with its text (up to eight lines). Return sends, Shift-Return breaks the line. In its bar:
  **the model** (a menu of the models your key can use that think adaptively), **Thinking** (low to max: how much
  Claude thinks before it answers, and how hard it works) and, on Claude Opus 5.5, **Fast** (the API's fast mode: the
  same model writing its answers up to 2.5 times as fast, at twice the price), all three machine-wide and shared by
  the two playground plugins (higher levels and larger models answer better and cost more), and the round **send**
  button, which turns into **stop** while Claude works (what it did stays). The line under the box says what Claude
  is doing, then the last reply's tokens: in (and how many were read from the cache) and out, and "fast" when fast
  mode wrote it. When fast mode stays busy a turn goes on at standard speed; when your key cannot use it, it turns
  itself off and says why.
- **The header**: the pencil starts a **new chat** (it asks first); the key opens the **API key** dialog: paste your
  key (from console.anthropic.com, API keys) with Cmd-V and Save. A message sent before a key is stored opens the
  same dialog and goes once the key is saved. Remove key forgets it.
- **Large work**: a complete synthesizer is a few hundred lines in several files (up to twelve tabs). Claude plans,
  takes building blocks from the library, writes its own files together, gives the dials a panel, runs them until they
  compile, then **measures** the result (a render made offline: levels, pitch, aliasing, the release and whether a
  voice ends, clicks, NaNs, the CPU cost; `Source/dsp/Measure.h`), since it cannot hear it, and sets the dials. An
  answer may be as long as the model allows (128K tokens), a turn may take 80 requests, a dropped connection or an
  overloaded server is asked again, and a long conversation is summarized by the API (compaction) when it nears 150K
  tokens, so it never runs out of room.
- **The session** keeps the conversation, so a project reopens with its chat; the key is never in it.
- **Network**: the chat is the only part of the plugin that uses the network, and only when you send a message: HTTPS
  to `api.anthropic.com` (the Messages API, streamed, and the models list once per key). Requests are billed to your
  key. A turn goes on while the editor is closed.

**Where the key is.** `~/Library/Application Support/Funk/Playground/anthropic-key.bin` (the folder 0700, the file
0600), shared by the effect and the synth playground, in any host and in their Standalones; `chat.json` beside it
keeps the model and the level. The file holds the key encrypted with AES-256-CBC and authenticated with HMAC-SHA256
(CommonCrypto), under keys derived with PBKDF2 (100,000 rounds, a fresh salt each save) from this Mac's device ID and
your account: copied to another Mac or read by another account it is useless, and nothing on disk shows the key in
clear. A program running as you on this Mac could derive the same keys, as it could read your other files: the
encryption protects the key at rest (backups, a copied folder, a stray search), not from such a program. To revoke a
key, delete it at console.anthropic.com.

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
| `fminus.contract` | what a program needs, and what the plugin says when it does not fit: no module `Voice`, no `start`, a `process` of another type (refused, never called), one that could allocate, a syntax error with its line; 32 dials and a note for the 33rd parameter; a held module's parameters at their defaults; init gets the rate; a module Shared made once, its init before every instance's (which reads what it made through a `Shared*` field), again at a new rate, read on the audio thread, a note when nothing points at it, a `Shared::init` of another type refused; several files: an included function plays, the main module in an included file, a file two files include read once, an included file's error with its name and include stack, a file not found, the plugin's message for a wrong `process` after an `#include` |
| `fminus.library` | every library file named on its first line, with a usage header, compiling alone and all together; each measured: the filters' responses (SVF and biquad -3 dB at their cutoffs, notches under -60 dB, the ladder's 24 dB/octave and its self-oscillation at the cutoff), the delay exact to the sample and its four-point read, the reverb's decay times, width and level, the oscillators' levels, DC and pitch and the saw's aliasing against a naive one, the noises' levels and colours, the envelope's times and its retrigger without a step, the LFO, the follower and the compressor's curve, the shapers' bounds, the crusher's steps and the conversions; the wavetable's shapes (RMS, no DC, pitch), nothing inharmonic above -80 dB from 1 to 11 kHz where a naive saw's aliases are above -40 dB, the 16th harmonic at full strength at the end of a fade, a glide's copies continuous and rising, a saw-to-square morph that keeps its fundamental, every call safe before build |
| `fminus.random` | RANDOM's programs, seeds 1 to 160: one program a seed, every seed's another; its first line names it and its seed, and it #includes its library files; each compiles with no message, at most sixteen dials, every one in a titled section; a chord held a second and released plays finite, audible and under 0 dBFS with no alert, every voice ended within five seconds; the first sixteen the same bits in blocks of 64 and 333; every part the generator can choose chosen by some seed |
| `fminus.panel` | the `#pragma panel` lines: sections, titles, order, a row break, labels and small knobs, comments and commas; an untitled section; the dials left over, last and noted; a note on its line for each line it cannot use (an unknown word, no such dial, a dial twice, an open quote, a missing label, an empty section, a title or label too long), the program still playing; lines in an included file, after the program's, and none from a file no `#include` reads; labels without a section; displays (each kind, of `out` and the voice's fields: name, `as`, label, range, the fields' taps in order; a display in a section and one left over; notes for a field that is not there, an array, a dial as a source, a name taken, a range from high to low, an `in`, which an instrument lacks); styles where they suit and a note where not, several dials a line; options; colours; displays without a section line |
| `dsp.swap` | the Engine: a held note's voice fades out when another program arrives, which starts the key again; seventeen notes steal the oldest; a voice that returns false is free; a real dial glides over 20 ms sample by sample, an integer one steps at the block; a NaN, an infinity and 10 leave as 0, 0 and 4, with their alerts |
| `dsp.meters` | what the Engine publishes for the displays: the output's peaks and RMS, left and right, within 1 % (no input: 0); the peaks falling 10 dB in half a second and the RMS 40 dB; the fields the displays show (a float, an int, a bool, as written last) from the newest sounding voice, then the one left, then none; the output's samples bit for bit; the frame count; the program the fields are of, not one offered and not yet taken |
| `plugin.process` | the processor as a host drives it: P1 to P32 named and worded as the program's, host writes reach the program (within 0.05 dB), a host's NaN ignored (its text a dash), a block of 0 samples (with a note-on in it) and a block larger than announced change nothing, the bypass is silent and its edges do not click, a block before prepareToPlay is silent; a note-on at velocity 0 and All Notes Off release |
| `plugin.presets` | the presets, in a folder of the probe's own (under CTest the default is the sandbox's): names tidied; a program of two files and its two dials saved, listed with a date, the JSON naming this plugin and its kind; loaded into a fresh processor, its files, tab and dials back, by name in any order (an unknown one ignored); a name in another case replaces it; the session keeps the name; another kind's preset, a damaged file and a missing name refused, the draft untouched; deleted, gone |
| `plugin.state` | the session round-trips (a program of two files, the draft's three files and its tab, the dials, the output bit for bit); a run moves a renamed parameter's dial to its default and keeps a kept one's; a session whose program does not compile leaves the last one playing; one without a program plays the first example; one saved before files loads as one file; one with too many files or bad names loads tidied; a NaN dial loads as 0.5 |
| `plugin.chatstore` | the key on disk, in a folder of the probe's own: a key's shape; saved, the file is the format above with no 12 characters of the key in it, 0600 in a 0700 folder, and loads back; saved twice it differs (fresh salt and IV); one byte changed anywhere, or the file cut short, and it does not load; forgotten, it is gone; the model, level and fast mode round-trip, a damaged chat.json loads as none |
| `plugin.chat` | the chat's agent against a scripted API (no network): the event stream however it is cut, a message rebuilt (thinking and its signature, text, a tool call's streamed input, a compaction block, the usage over its iterations); no key, no request; a turn that writes a voice, runs it, sets its dial and answers; each request's shape (model, the model's own max_tokens, adaptive thinking, effort, the cached system prompt, the ten tools, fallbacks and compaction with their betas, the API version) and the history append-only; another model and level; fast mode (`speed` and no fallbacks, the footer's "fast", standard speed when it stays busy, off when the API refuses it, the beta on Opus 4.8, none on Sonnet); a tool input that is not JSON; a retry after 529; a 401; a refusal; a fallback; a file Claude has seen named but not resent; measure_output through the agent; add_library_file (the tab, its text in the result and seen after it, an unknown name, the same file twice); a stream cut short asked again and shown once; a compaction (the history starting at the summary, the files sent again), in the middle of a turn too (every tool result with its call, the files with the results), and a session saved with the old cut repaired; STOP; the session; NEW |
| `plugin.chatexample` | the system prompt has the large-program section, the panel's lines, every library file's header and the program built from the library, word for word; that program compiles with no message (its panel too), every dial in a titled section, plays finite and audible, the same in blocks of 64 and 333, its voices ending; measure_output's numbers on programs whose answer is known (a level, a pitch, a gain, an impulse), and its notes for a voice that never ends or an effect that makes sound from silence; the Inharmonic line (a sine's clean; at C7 a naive saw's aliases above -40 dB and a wavetable voice's clean, its table built once in `Shared`); a saw's own edges no click, a phase that jumps once a click; the prompt's FFT and `Shared` |
| `gui.chat` | the chat column and resizing, headless, to `gui.chat.png`, `.empty.png`, `.key.png`, `.paper.png`, `.small.png` and `.large.png`: the empty chat (key button, suggestions); the key dialog (opened by a message with no key, Escape, modal, a key refused, pasted, never spoken, Save sends the waiting message, Remove); the message box growing with its rows and shrinking when sent; a turn shown (thinking that opens and closes, what Claude did, the answer, a code block and its Copy, an answer's Copy, the usage), its files as tabs, the program playing, Cmd-Z undoing Claude's change; the model and thinking menus; the Fast switch (on Claude Opus 5.5 only, a switch to a screen reader); stop; new chat; a suggestion into the box, unsent; the wheel and the jump to the end; the least size with twelve narrowed tabs and a large size, every item inside; the session keeps the size |
| `gui.dump` | the panel headless to `gui.dump.png`, `.paper.png`, `.error.png`, `.edited.png`, `.files.png`, `.dials.png`, `.dials.small.png`, `.dials.most.png`, `.dials.most.small.png`, `.panel.png`, `.panel.paper.png`, `.panel.small.png`, `.example.png`, `.displays.png`, `.displays.paper.png`, `.random.png` and `.presets.png`: settles, every glyph, the draft in the editor and a dial for each parameter; typing, Cmd-Return, a syntax error (status, problem line, gutter mark, the click that jumps to it), the EXAMPLES menu and its undo; the files' tabs (+, renaming in place, a taken name refused, Escape, typing into a second file, an `#include` of it that runs, its error's problem and marks, the click that shows its file, closing with and without asking); a dial's drag and double-click; sixteen dials in rows (two at the default and the least size, one when wide; in order, evenly spaced, inside the left column, the editor giving up a row's height; 32 in three rows, every knob small at the least size); sixteen dials in five sections (two rows of frames, titles in order, every dial inside its frame, the rows even, the labels and small knobs asked for, at the least size too; a bad panel line noted on its line); the LIBRARY menu (every file under its heading; a file added as a tab and #included after the program's description, undoable, still running; ticked and not added twice; greyed with twelve tabs); the program Claude's instructions build from the library, its files added through the menu's path, running with no note, every dial in a titled section; a program with every display and dial style, coloured, a note held through the processor (each item in its frame; a meter at the Engine's peak within 0.3 dB, a field's meter, light, number and trace; a button's click chooses its value, the switch flips, a fader's drag raises its dial, the host says an option's name; another program's light dark until the audio thread plays it); RANDOM (a seed's program in place of the text, its library files as tabs, run with no note, Cmd-Z back in one step; the button's another; with twelve tabs, a notice and nothing changed); PRESETS (the browser opened with the program's title, a name typed and saved: the status and the header say so, EDITED after an edit; listed by name, Load asking over changes and then loading the text back, Save asking before it replaces, Delete asking; Escape); Escape and Tab |
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
Source/fminus/           Patch: a program compiled for the plugin (the contract, the calls, the instances, the fields
                         its displays read), JUCE-free;
                         PanelSpec (its #pragma panel lines read: sections, dial styles, displays);
                         Random (RANDOM: a program put together from the library's building blocks);
                         examples/*.fm, the EXAMPLES (compiled in by CMakeLists.txt as generated/Examples.h);
                         library/<n>-<category>/*.fm, the LIBRARY (as generated/Library.h)
Source/dsp/              the audio side, plain C++20 without JUCE: Engine (the voices, programs swapped without a
                         click, the dials glided into streams, the output guard, the levels, fields and scope the
                         displays read), Compiler (compiles off the audio thread), Controls, Events, Sources (a
                         program's files), Measure (a program rendered offline and measured, for the chat)
Source/fsdsp/core/       FSynth's DSP core, vendored at the tag in SEED.tsv (scripts/sync-dsp-core.sh)
Source/plugin/           the JUCE AudioProcessor: MIDI to events, P1 to P32, the program's files and the draft in the
                         session, the bypass; EditorPort.h, what the editor reaches besides the parameters;
                         the chat: ChatAgent (the conversation, its turns and Claude's tools), ChatPrompt.cpp (Claude's
                         instructions: the panel, the library, a program built from it), ClaudeApi (the Messages API
                         over HTTPS),
                         ChatStore (the key, encrypted), ChatPort.h (what the editor reaches of it);
                         PresetStore (the presets on this Mac)
Source/editor/           the FunkGui panel (Panel.cpp: the toolbar, the files' tabs and TextEditors, the problems, the
                         dials, the chat's card and the key dialog; its layout follows the window's size),
                         DialLayout (the dials' and displays' places: plain rows, or the panel's sections),
                         PresetBrowser (PRESETS' card: the list, the name, Save, Load and Delete),
                         Looks (the dial styles and the displays drawn: vintage knobs, faders, switches, buttons, LED
                         meters, a VU, lights, readouts, a scope), DisplayItem (a display's motion: VU ballistics,
                         peak hold),
                         ChatView (the conversation), ChatItems (the message box, the chat's buttons), ChatStyle.h
                         (the chat's rounded look, its palette from the theme), Items.h, Theme.h;
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
