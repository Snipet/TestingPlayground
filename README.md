# TestingPlayground

An effect (AU, VST3 and Standalone, macOS on Apple silicon) whose DSP you write while it runs: its editor is a text
field holding an [FMinus](https://github.com/Snipet/FMinus) program, the plugin compiles it when you press RUN (or
Cmd-Return), and the next block plays it, crossfaded from the last. The program's parameters become the dials under
the text. A chat with Claude at the right of the editor writes and changes the program for you: describe an effect
and it writes the FMinus, runs it, fixes what does not compile and leaves the dials at good settings, so a sentence
does what a preset did. Built on JUCE 8.0.4, with a GPU editor made with [FunkGui](https://github.com/Snipet/FunkGui)
(its `TextEditor`) and FMinus, the JIT compiler for DSP, linked in.

<!-- origin -->
It began as a feedback delay network reverb (made from FunkPluginTemplate, its FDN ported from the Visage version,
git tag `legacy-visage`); that reverb, and the file-watching script that ran after it, are in the history before the
branch `fminus-editor`.
<!-- /origin -->

After a build, `build-dev/probe-out/gui.dump.png` is a picture of the editor.

## Writing a program

FMinus is C with DSP's additions (vectors, modules, parameters); its README has the language, and its
`docs/programs.md` and `docs/extending.md` the details. The plugin plays a module named `Effect`:

```c
module Effect
{
    param float gain [[fminus::range(-24, 12)]] [[fminus::default(0)]];   // dB: a dial
    float level;                                                          // state: the module's fields

    void init(float rate)                     // optional: before the first block, and when the sample rate changes
    {
        level = 1;
    }

    void process(float* restrict left, float* restrict right, int n)     // the audio thread: in place
    {
        for (int i = 0; i < n; ++i)
        {
            left[i] *= level * expf(0.115129255f * gain[i]);
            right[i] *= level * expf(0.115129255f * gain[i]);
        }
    }

    void end() {}                             // optional: before the program goes (free what init allocated)
};
```

- **process** gets n samples of both channels and changes them in place (a mono bus: `right` is a copy of the left,
  unheard). A module's fields are its state: they start at 0, live as long as the program, and carry from block to
  block. Keep the state per sample, and the output does not depend on the host's block size.
- **Dials.** The module's first 32 parameters, in the order they are declared, are the dials, and the host's
  parameters P1 to P32 (a host shows "1 gain", and the value in the program's units). A `float` glides when its dial
  turns: it is read sample by sample, `gain[i]`, as a stream of 20 ms ramps, and held still otherwise. An `int` or a
  `bool` is read once a call (`mode[0]`, or any index). `fminus::range` sets the dial's span (a float without one
  spans 0 to 1, an int 0 to 10) and `fminus::default` where it starts and where a double-click returns it. A run
  that brings a parameter the slot did not have moves that dial to its default; one with the same name keeps it.
- **The audio thread.** process may not allocate: FMinus refuses a program whose process could reach `calloc`,
  `malloc` or `free`, and names the line. Allocate in init (a delay line: `calloc`, zeroed) and free in end, both
  off the audio thread; or use a fixed array, `float line[48000];`, a field like any other.
- **Modules inside modules.** The main module may hold others (`Svf l;`, `Osc osc[4];`): their methods are its
  helpers, and their parameters start at their defaults (they have no dials).
- **Shared data.** A module named `Shared`, when the program has one, is made once for it: its init runs before the
  effect's (with the rate), its end after, and every field of Effect of type `Shared*` points at it. The synth
  playground's voices share a wavetable this way; an effect, one instance, may as well hold what it needs.
- **The FFT.** FMinus has one, real and complex: a plan from `fft_create(n)` in init (n a multiple of 32 made of 2s,
  3s and 5s: 256, 1024, 2048), `fft_forward(plan, in, bins)` and `fft_inverse(plan, bins, out)` anywhere, the audio
  thread too (n floats to n / 2 + 1 bins and back, unscaled: the round trip is n times the input), `fft_destroy(plan)`
  in end. `wavetable.fm` builds band-limited wavetables with it.
- **Several files.** The editor has a tab for each file, the program's first (`program.fm`). The program includes
  another by its name, `#include "filters.fm"`, as C does: the file is read where the line is (a module there is a
  type after it), and once only, as if every file began with `#pragma once`, so two files may both include a third.
  Its errors name it ("filters.fm:3:12: error: ..."). FMinus has no other directive: no macros, no `#if`.
- **The library.** Ready-made building blocks, tested by `fminus.library`: filters (`svf.fm`, `ladder.fm`,
  `onepole.fm`, `biquad.fm`), delays and reverb (`delay.fm`, `reverb.fm`), sources (`osc.fm`, `wavetable.fm`,
  `noise.fm`), modulation (`adsr.fm`, `lfo.fm`, `follower.fm`), shaping and utilities (`shaper.fm`, `util.fm`).
  LIBRARY (the editor) or Claude adds one as a tab of the program's own; `#include` it and use its modules (each
  file's header says how). It is then the program's file like any other, saved with it, and yours to change. They
  live in `Source/fminus/library/<n>-<category>/`.
- **The interface: a .gui file.** A file named `panel.gui` (any name ending in `.gui`) beside the program gives it
  an interface richer than the `#pragma panel` lines below, which it replaces: containers laid out as CSS's flexbox
  lays them out, pages behind tabs, every kind of control with its own label, unit and taper, displays and text
  (FMinus never reads it; `Source/fminus/GuiSpec.h` has the language, `Source/editor/GuiLayout.h` the layout):

  ```
  panel accent=orange {                         // around everything: accent=, background=
      row gap=10 {                              // left to right; column: top to bottom; grid cols=4
          section "FILTER" color=orange {       // a frame, its title in the top edge (layout=column stacks)
              knob cutoff size=large taper=log unit="Hz"   // small, medium, large; style=vintage
              knob resonance label="RESO"
          }
          section "ENVELOPE" {
              slider attack; slider decay; slider sustain scale=100 unit="%" decimals=0; slider release
              envelope attack decay sustain release          // their shape, drawn
          }
          box {                                 // a filled frame without a title
              buttons wave "SAW" "SQR" "TRI" horizontal
              switch sync "OFF" "ON"
              menu mode "POLY" "MONO"
          }
      }
      tabs {                                    // pages, one shown at a time
          page "MOD" { xy level pan }           // a pad that moves two parameters
          page "METERS" { meter out; vu out; led gate; number count; scope out; spectrum out grow=1 }
      }
  }
  ```

  An element is a line (or several, between `;`): its name, its arguments, `key=value` properties, its children
  between `{` and `}`; `//` and `/* */` are comments. Every container takes `gap=`, `pad=`, `align=` (stretch, start,
  center, end), `justify=` (start, center, end, between, around), `grow=`, `width=` and `height=`; `spacer`,
  `divider` and `label "TEXT" size=title` fill and dress it. `taper=log` gives a frequency's or a time's knob the same
  turn for each octave, and the host's automation follows it; `unit=`, `decimals=` and `scale=` say how a value reads,
  to the host too (`Source/dsp/Controls.h`). Displays are those of the lines below and `spectrum` (an analyser of `in`
  or `out`), sixteen at most. What the plugin cannot use is a note on its line and column ("panel.gui:12:5: note:
  ..."), and the rest stands; a parameter no control names comes last, in a section without a title.
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
  - **Displays** show the plugin's audio, `in` (an effect's input) or `out` (the output; left and right, in dBFS), or
    a float, int or bool field of the main module that process writes (`gr = reduction;`), across a range given
    after it (0 to 1 unless it says: `#pragma panel meter gr 0 24 "GR"`). `meter`: LED bars, the highest held a
    second (the audio's peaks); `vu`: an analog VU meter's needle (0 VU at -18 dBFS RMS); `led`: a light, a field's
    value its brightness (of the audio: a clip light); `number`: a readout of the field (of the audio: its peak in
    dBFS); `scope`: an oscilloscope (of a field: its trace over the last 2.5 seconds). An instrument's fields are its
    newest sounding voice's. A section names a display by its source, or by the name `as` gives it, which a second
    display of one source needs: `#pragma panel scope out as wave "WAVE"`. Eight displays at most. The colours: red,
    orange, yellow, green, teal, blue, violet, pink, cream, grey.

  A row too wide for the window wraps, its sections shared out evenly; shorter rows widen toward the widest; when
  the sections need more height than the window gives, every knob is drawn small. Dials and displays no section
  names come last, untitled. A line the plugin cannot use (a name that is no dial, a dial in two sections, a field
  that is not there, a style the dial does not suit) is a note on its line, and the rest stands
  (`Source/fminus/PanelSpec.h`). The examples and RANDOM's programs use these lines; Claude writes a `panel.gui`.
- **Its types are checked.** The plugin calls each method through a line of its own with the type it calls it by, so
  a `process` with other parameters is an error ("Effect::process: the plugin calls it as ..."), never a crash.
- **What the plugin guards, and what it cannot.** Every sample that leaves the plugin is finite and within +-4
  (+12 dBFS): a NaN or an infinity becomes 0 and a louder sample is clipped, and the toolbar says so. A NaN in the
  program's state stays there: RUN again starts it from scratch. But the program is native code: a write through a
  pointer past its memory, or a loop that never ends, takes the host with it. Try a new program in the Standalone.
- **Subnormals** are flushed to zero (the plugin compiles with FMinus's `Denormals::Flush`, the mode the audio thread
  runs in), and the math builtins (`sinf`, `cosf`, `expf`, `tanhf`, `logf`; also `fabsf`, `sqrtf`, `floorf`, `fminf`,
  `fmaxf`) are FMinus's default math mode's: one result for a float wherever it is computed.

The EXAMPLES menu holds five to start from (`Source/fminus/examples/`): Gain and pan (the program a new instance
plays), Tremolo (init and the sample rate), Filter (a state-variable filter as a module of its own), Echo (a delay line
from calloc in init, freed in end; a VU and a switch) and Drive (tanh saturation; input and output meters, a vintage
knob). To add one, add a file there whose first line is
`// <Title>: what it is`.

## The editor

- **RUN** compiles the text and plays it once it compiles, crossfaded over 20 ms; **Cmd-Return** does the same from
  the keyboard. Compiling takes some tens of milliseconds, off the audio thread and off the editor's.
- **EXAMPLES** replaces the program's text with an example (Cmd-Z gives yours back) and runs it; the other files
  stay.
- **LIBRARY** lists the library's files by category, a tick on those the program has. One it lacks becomes a tab,
  shown, and the program `#include`s it (a line after its description; Cmd-Z in the program takes it back); one it
  has is shown. With twelve tabs, the rest are greyed.
- **RANDOM** writes a new effect for inspiration, from the library's building blocks: two to four of a drive (tanh,
  a wavefolder, a crusher or a tube's curve), a filter (still, swept by an LFO, or opened by the input's level), a
  chorus, a flanger, a tremolo or an auto-pan, and an echo or a reverb, then a dry/wet mix, with random settings, a
  panel and a name. It replaces the program's text (Cmd-Z gives yours back), adds the library files it needs as
  tabs, and runs it; each click makes another. Its first line names it and its seed (`random 4821`).
- **PRESETS** keeps programs on this Mac: the editor's files (every tab, and the one shown) and the dials' values,
  under a name, in `~/Library/Audio/Presets/Funk/TestingPlayground/` (a JSON file a preset, `<name>.fmpreset`: copy,
  rename or share them in the Finder). The browser lists them with when each was saved: a click chooses one, a
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
- **CODE and PANEL**, in the toolbar: CODE shows the editor with the program's interface under it (a .gui file's takes
  half the column at most and scrolls with the wheel past that; over a control the wheel turns it); PANEL gives the
  interface the whole left column, the editor out of the way. The session keeps the choice.
- **The dials** are the program's parameters (above): in the sections its `#pragma panel` lines ask for (above),
  in their styles and beside its displays, or in order, in as few rows as the width holds (sixteen in two rows of
  eight at the default size, in one row on a wide window, 32 in three; every knob small when the window is too short
  for them); drag, scroll, or use the keys once Tab has focused one. A switch or a button takes a click (Return or
  Space steps it). The displays move with the sound, as hardware does. A program with a `panel.gui` has its interface instead
  (above): knobs, sliders and the XY pad take a drag (Shift is finer), the wheel and the keys, and a double-click for
  the default; buttons, switches and menus a click; a tab shows its page.
- **The session** keeps the program that plays (all its files), the files as you left them (run or not), the tab
  shown, the dials and the preset's name. A session whose program no longer compiles (an older FMinus, say) leaves the
  first example playing, its files in the editor. A session saved before files loads as a program of one file.
- **The window** resizes in both directions, from 1000 x 600 (logical px, before ZOOM) up: the editor and the chat's
  conversation take the height, the chat about a third of the width. The session keeps the size.
- THEME (header) and ZOOM (footer) are machine-wide preferences.

## The chat

The rounded card at the right is a conversation with Claude, through the [Claude API](https://docs.claude.com): sign in
with a TestingPlayground account (Claude then runs on the TestingPlayground server, and what it costs comes out of the
account's credit), or use your own Anthropic API key (billed to you). Claude sees the plugin's files, whether they play,
the last compile's messages and the dials (sent with a message when they changed; a file it has already seen goes as one
line, not again in full), and it can act on them: read, write, edit, rename and delete the files (the editor's tabs
follow, and Cmd-Z in a file undoes its change), add the library's building blocks, run the program (RUN; the compile's
messages come back to it), set the dials, measure what the program puts out, and read the CPU load and the output
guard's alerts. Ask it for an effect ("a tape delay with wow and flutter"), a change ("make the filter resonate more"),
an explanation ("what does this program do?"), or help with an error or a click. It knows FMinus, the plugin's contract,
the library (each file's header) and how to build a large program from it, and it lays the dials out in a panel (its
instructions are in `Source/plugin/ChatPrompt.cpp`).

- **The conversation**: your messages in bubbles at the right; Claude's answers in light markdown (lists, headings,
  **bold**, `code`, and code blocks with a **Copy** of their own); a **Copy** under each finished answer (it shows
  under the pointer); Claude's thinking as a row that opens and closes; what it did (a file written, a run, a dial
  set, a sound measured) as a card of rows; errors in a tinted card. While Claude works, pulsing dots say what it is
  doing ("Writing osc.fm (12 KB)"). The wheel scrolls; a round button brings you back to the end. An empty chat
  offers suggestions: a click puts one in the message box, and nothing is sent until you send it.
- **The message box** grows with its text (up to eight lines). Return sends, Shift-Return breaks the line. In its bar:
  **the model** (a menu of the models your key or your account can use that think adaptively), **Thinking** (low to max:
  how much Claude thinks before it answers, and how hard it works) and, on Claude Opus 5.5, **Fast** (the API's fast
  mode: the same model writing its answers up to 2.5 times as fast, at twice the price), all three machine-wide and
  shared by the two playground plugins (higher levels and larger models answer better and cost more), and the round
  **send** button, which turns into **stop** while Claude works (what it did stays). The line under the box says what
  Claude is doing, then the last reply's tokens: in (and how many were read from the cache) and out, "fast" when fast
  mode wrote it, and, signed in, the credit left ("$4.50 credit left"). When fast mode stays busy a turn goes on at
  standard speed; when your key cannot use it, it turns itself off and says why.
- **The header**: signed in, your username and the credit left (a click opens your account); the pencil starts a
  **new chat** (it asks first); the key opens the **access** dialog, which has two tabs (a dot marks the one the chat
  uses):
  - **Account**: your username and password (from the TestingPlayground admin; Tab moves to the password, Cmd-V
    pastes it) and **Sign in**. Signed in, it is your account: who you are, the credit left, this month's requests,
    spending and tokens, when you signed in and from which plugin, when the sign-in ends; **Refresh** asks the server
    again (opening the tab does too), and **Sign out**. The password is
    sent once, to the server, and kept nowhere; the server answers with a sign-in that lasts 90 days from its last
    use. When it ends (it expired, or the admin signed you out) the chat says so and asks you to sign in again; when
    the credit is used up it says that (ask the admin for more).
  - **API key**: paste your key (from console.anthropic.com, API keys) with Cmd-V and **Save**. **Remove key**
    forgets it.

  Both can be stored at once: the chat uses the one you set up last, and the other tab offers to switch back to it
  (**Use account**, **Use this key**). The empty chat has a **Sign in** and a **Use an API key** button, and a message
  sent before the chat can send it opens the dialog and goes once you have signed in or saved a key.
- **Large work**: a complete synthesizer is a few hundred lines in several files (up to twelve tabs). Claude plans,
  takes building blocks from the library, writes its own files together, gives the dials a panel, runs them until
  they compile, then **measures** the result (a render made offline:
  levels, pitch, the release and whether a voice ends, clicks, NaNs, the CPU cost; `Source/dsp/Measure.h`), since it
  cannot hear it, and sets the dials. An answer may be as long as the model allows (128K tokens, its thinking
  included), a turn may take 80 requests, a dropped connection or an overloaded server is asked again, and a long
  conversation is summarized by the API (compaction) when it nears 150K tokens, so it never runs out of room. An
  answer cut off at its limit goes on by itself, Claude told why; a second one in a row ends the turn (send
  "continue" to go on). At **Thinking: Max** (and XHigh) Claude thinks the longest, so a hard design is more likely
  to reach the limit: **High** leaves it more room to write.
- **The session** keeps the conversation, so a project reopens with its chat; neither the key nor the sign-in is in
  it.
- **Network**: the chat is the only part of the plugin that uses the network, and only when you send a message, sign
  in or sign out. With your key: HTTPS to `api.anthropic.com` (the Messages API, streamed, and the models list once
  per key), billed to your key. Signed in: the same requests over HTTPS to the TestingPlayground server, which passes
  them on to Anthropic unchanged and charges each to your account's credit (it lists only the models it can charge
  for), and after each turn it is asked for the credit left. A turn goes on while the editor is closed. For a server
  run locally (`wrangler dev`), start the plugin with `TSTN_SERVER_URL=http://localhost:8787` (only https://
  addresses, or http:// on this Mac, are taken).

**Where the key is.** `~/Library/Application Support/Funk/Playground/anthropic-key.bin` (the folder 0700, the file
0600), shared by the effect and the synth playground, in any host and in their Standalones; the sign-in (the token the
server gave, never the password) is `account-token.bin` beside it, kept the same way; `chat.json` keeps the model, the
level, the username and which of the two the chat uses. The file holds the key encrypted with AES-256-CBC and
authenticated with HMAC-SHA256 (CommonCrypto), under keys derived with PBKDF2 (100,000 rounds, a fresh salt each save)
from this Mac's device ID and your account: copied to another Mac or read by another account it is useless, and nothing
on disk shows the key in clear. A program running as you on this Mac could derive the same keys, as it could read your
other files: the encryption protects the key at rest (backups, a copied folder, a stray search), not from such a
program. To revoke a key, delete it at console.anthropic.com; Sign out ends the sign-in on the server too.

## Build and run

Requirements: macOS 14 or later on Apple silicon, Xcode or its command-line tools, CMake 3.30 or later, Ninja,
Python 3. JUCE, bgfx and the shader compiler come from the machine cache `~/audio/.deps` (FCompressor's
`Scripts/deps.sh` fills it), FunkGui from `~/audio/libraries/FunkGui` at the pinned commit (`cmake/Deps.cmake`); a
machine without them fetches them from GitHub instead. FMinus comes from `~/audio/libraries/FMinus` at the pinned
commit, with the LLVM, Bison and Flex its own build left in its deps cache: build FMinus once first ("FMinus" below).

```sh
cmake --workflow --preset dev-verify     # configure, build everything, run every test (build-dev/)
open build-dev/TestingPlayground_artefacts/RelWithDebInfo/Standalone/TestingPlayground.app
```

In the Standalone, the Options button in the title bar picks the audio input and output. It starts with its input
muted (JUCE's guard against feedback): unmute it there, with headphones on. For a host:

```sh
cmake --workflow --preset release-build  # Release with LTO (build-release/)
scripts/install.sh                       # copy the AU and VST3 into ~/Library/Audio/Plug-Ins
```

Builds never install anything by themselves; `scripts/install.sh build-dev` installs a dev build instead. The plugin
code in `cmake/Product.cmake` (`scripts/plugin.py info` prints the whole identity) is this plugin's own, so it loads
in one host beside your other plugins. A host that keeps keys for itself (some do for the space bar or the arrows)
may need its "keyboard to plugin" option on for typing in the editor.

## Test

```sh
scripts/test.sh                       # build the probes, run them; outputs in build-dev/probe-out/
scripts/test.sh -R fminus.contract -V # one probe, every row printed
build-dev/TestingPlayground_probes --list
```

| Probe | Checks |
| --- | --- |
| `fminus.examples` | every example compiles with no message, its dials named, with ranges and defaults inside them; plays two seconds over noise, finite, audible and with no alert; the same bits in blocks of 64 and 333 |
| `fminus.contract` | what a program needs, and what the plugin says when it does not fit: no module `Effect`, no `process`, a `process` of another type (refused, never called), one that could allocate, a syntax error with its line; 32 dials and a note for the 33rd parameter; a held module's parameters at their defaults; init gets the rate; a module Shared made once, its init before the effect's (which reads what it made through a `Shared*` field), again at a new rate, read on the audio thread, a note when nothing points at it, a `Shared::init` of another type refused; several files: an included function plays, the main module in an included file, a file two files include read once, an included file's error with its name and include stack, a file not found, the plugin's message for a wrong `process` after an `#include` |
| `fminus.library` | every library file named on its first line, with a usage header, compiling alone and all together; each measured: the filters' responses (SVF and biquad -3 dB at their cutoffs, notches under -60 dB, the ladder's 24 dB/octave and its self-oscillation at the cutoff), the delay exact to the sample and its four-point read, the reverb's decay times, width and level, the oscillators' levels, DC and pitch and the saw's aliasing against a naive one, the noises' levels and colours, the envelope's times and its retrigger without a step, the LFO, the follower and the compressor's curve, the shapers' bounds, the crusher's steps and the conversions; the wavetable's shapes (RMS, no DC, pitch), nothing inharmonic above -80 dB from 1 to 11 kHz where a naive saw's aliases are above -40 dB, the 16th harmonic at full strength at the end of a fade, a glide's copies continuous and rising, a saw-to-square morph that keeps its fundamental, every call safe before build |
| `fminus.random` | RANDOM's programs, seeds 1 to 160: one program a seed, every seed's another; its first line names it and its seed, and it #includes its library files; each compiles with no message, at most sixteen dials, every one in a titled section; noise and a sine through it play finite, audible and under +6 dBFS with no alert, and silence through a fresh one stays silent; the first sixteen the same bits in blocks of 64 and 333; every part the generator can choose chosen by some seed |
| `fminus.gui` | a `.gui` file: a rich file's every element in its tree (the panel's colours, containers and their properties, sections, a box, tabs and pages, labels, a divider, a spacer, every control and display), options and labels for the host; comments, `;`, a `{` on the next line, an escaped quote, `#rgb`; a log taper's middle (the geometric mean) and back, units (Hz to kHz), decimals and scale, a typed text back to its value; displays (kinds, taps, two of one source, a range); the parameters left out, last with a note; a note on its line and column for each thing it cannot use (an unknown element, property or value, no such parameter, one twice, a control that does not suit its parameter, a pad on an int, an envelope of three, a label without text, tabs without pages, a page outside tabs, a colour, a unit after a number, a log taper from 0, decimals out of range, a field that is not there, an open quote, a stray `}`, an unclosed `{`), the rest standing; a second `.gui` file and `#pragma panel` lines beside one, noted; pragmas alone as before |
| `fminus.panel` | the `#pragma panel` lines: sections, titles, order, a row break, labels and small knobs, comments and commas; an untitled section; the dials left over, last and noted; a note on its line for each line it cannot use (an unknown word, no such dial, a dial twice, an open quote, a missing label, an empty section, a title or label too long), the program still playing; lines in an included file, after the program's, and none from a file no `#include` reads; labels without a section; displays (each kind, of `in`, `out` and the main module's fields: name, `as`, label, range, the fields' taps in order; a display in a section and one left over; notes for a field that is not there, an array, a dial as a source, a name taken, a range from high to low, an instrument's `in`); styles where they suit and a note where not; options; colours; displays without a section line |
| `dsp.swap` | the Engine: no program passes the input; the first program fades in from it, a second crossfades from the first over 20 ms with no step beyond the signal's own, bit for bit outside the fades, and the old one is handed back; a real dial glides over 20 ms sample by sample, an integer one steps at the block; a NaN, an infinity and 10 leave as 0, 0 and 4, with their alerts |
| `dsp.meters` | what the Engine publishes for the displays: an effect's input and output peaks and RMS, left and right, within 1 % (an instrument's output); the peaks falling 10 dB in half a second and the RMS 40 dB; the fields the displays show (a float, an int, a bool, as written last; an instrument's from its newest sounding voice, then the one left, then none); the scope's samples bit for bit; the frame count; the program the fields are of, not one offered and not yet taken |
| `plugin.process` | the processor as a host drives it: P1 to P32 named and worded as the program's, host writes reach the program (within 0.05 dB), a host's NaN ignored (its text a dash), a block of 0 samples and a block larger than announced change nothing, the bypass passes the input bit for bit and its edges do not click, a block before prepareToPlay passes the input with NaN and inf as 0 |
| `plugin.presets` | the presets, in a folder of the probe's own (under CTest the default is the sandbox's): names tidied; a program of two files and its two dials saved, listed with a date, the JSON naming this plugin and its kind; loaded into a fresh processor, its files, tab and dials back, by name in any order (an unknown one ignored); a name in another case replaces it; the session keeps the name; another kind's preset, a damaged file and a missing name refused, the draft untouched; deleted, gone |
| `plugin.state` | the session round-trips (a program of two files, the draft's three files and its tab, the dials, the output bit for bit); a run moves a renamed parameter's dial to its default and keeps a kept one's; a session whose program does not compile leaves the last one playing; one without a program plays the first example; one saved before files loads as one file; one with too many files or bad names loads tidied; a NaN dial loads as 0.5 |
| `plugin.chatstore` | the key on disk, in a folder of the probe's own: a key's shape; saved, the file is the format above with no 12 characters of the key in it, 0600 in a 0700 folder, and loads back; saved twice it differs (fresh salt and IV); one byte changed anywhere, or the file cut short, and it does not load; forgotten, it is gone; the sign-in token the same way (its shape, the file, a byte changed, forgotten without the key); the model, level and fast mode round-trip, a damaged chat.json loads as none; the username and the access in use round-trip beside them, and never chosen the chat uses the account unless only a key is stored |
| `plugin.chat` | the chat's agent against a scripted API (no network): the event stream however it is cut, a message rebuilt (thinking and its signature, text, a tool call's streamed input, a compaction block, the usage over its iterations); no key, no request; a turn that writes a program, runs it, sets its dial and answers; each request's shape (model, the model's own max_tokens, adaptive thinking, effort, the cached system prompt, the ten tools, fallbacks and compaction with their betas, the API version) and the history append-only; another model and level; fast mode (`speed` and no fallbacks, the footer's "fast", standard speed when it stays busy, off when the API refuses it, the beta on Opus 4.8, none on Sonnet); a tool input that is not JSON; an answer cut off at its limit (the turn goes on, Claude told; twice in a row it ends; a tool call cut off runs nothing; the limit note at max effort, none at medium); a retry after 529; a 401; a refusal; a fallback; a file Claude has seen named but not resent; measure_output through the agent; add_library_file (the tab, its text in the result and seen after it, an unknown name, the same file twice); a stream cut short asked again and shown once; a compaction (the history starting at the summary, the files sent again), in the middle of a turn too (every tool result with its call, the files with the results), and a session saved with the old cut repaired; a session saved with another system prompt or none loading without its thinking; STOP; the session; signing in to a scripted server (refusals shown and nothing stored, the token stored, the account in use; a turn sent to the server with the token as a bearer and no key, the models asked of it, the credit after the turn; no token or password in the session or a request body; its 402, 502, 400 and 403 shown and not retried, a 529 retried; a 401 forgetting the token and sending nothing after; the key used again; the one set up last in use; signing out; the server's address from the environment); NEW |
| `plugin.chatexample` | the system prompt has the large-program section, the .gui file's language (THE INTERFACE), every library file's header and the program built from the library and its `panel.gui`, word for word; that program compiles with no message (its interface too), every dial a control in a titled section, two displays, plays finite and audible, the same in blocks of 64 and 333, its voices ending; measure_output's numbers on programs whose answer is known (a level, a pitch, a gain, an impulse), and its notes for a voice that never ends or an effect that makes sound from silence; the prompt's FFT and `Shared` |
| `gui.chat` | the chat column and resizing, headless, to `gui.chat.png`, `.empty.png`, `.key.png`, `.signin.png`, `.account.png`, `.paper.png`, `.small.png` and `.large.png`: the empty chat (Sign in and API key buttons, suggestions); the access dialog's API key tab (opened by a message the chat cannot send yet, Escape, modal, a key refused, pasted, never spoken, Save sends the waiting message, Remove); its account tab (a username typed, a password pasted and never spoken, a wrong password, signed in, a message through the server, the credit, the header's account pill opening the account (asked of the server again: this month's requests, spending and tokens, the sign-in's dates, spoken too), Refresh, Use this key and Use account, Sign out); the message box growing with its rows and shrinking when sent; a turn shown (thinking that opens and closes, what Claude did, the answer, a code block and its Copy, an answer's Copy, the usage), its files as tabs, the program playing, Cmd-Z undoing Claude's change; the model and thinking menus; the Fast switch (on Claude Opus 5.5 only, a switch to a screen reader); stop; new chat; a suggestion into the box, unsent; the wheel and the jump to the end; the least size with twelve narrowed tabs and a large size, every item inside; the session keeps the size |
| `gui.dump` | the panel headless to `gui.dump.png`, `.paper.png`, `.error.png`, `.edited.png`, `.files.png`, `.dials.png`, `.dials.small.png`, `.dials.most.png`, `.dials.most.small.png`, `.panel.png`, `.panel.paper.png`, `.panel.small.png`, `.example.png`, `.displays.png`, `.displays.paper.png`, `.random.png` and `.presets.png`: settles, every glyph, the draft in the editor and a dial for each parameter; typing, Cmd-Return, a syntax error (status, problem line, gutter mark, the click that jumps to it), the EXAMPLES menu and its undo; the files' tabs (+, renaming in place, a taken name refused, Escape, typing into a second file, an `#include` of it that runs, its error's problem and marks, the click that shows its file, closing with and without asking); a dial's drag and double-click; sixteen dials in rows (two at the default and the least size, one when wide; in order, evenly spaced, inside the left column, the editor giving up a row's height; 32 in three rows, every knob small at the least size); sixteen dials in five sections (two rows of frames, titles in order, every dial inside its frame, the rows even, the labels and small knobs asked for, at the least size too; a bad panel line noted on its line); the LIBRARY menu (every file under its heading; a file added as a tab and #included after the program's description, undoable, still running; ticked and not added twice; greyed with twelve tabs); the program Claude's instructions build from the library, its files added through the menu's path and its `panel.gui` in a tab of its own, running with no note, its interface's every dial in a titled section; a program with every display and dial style, coloured, audio through the processor (each item in its frame; a meter at the Engine's peak within 0.3 dB, a field's meter, light, number and trace; a button's click chooses its value, the switch flips, a fader's drag raises its dial, the host says an option's name; another program's light dark until the audio thread plays it); RANDOM (a seed's program in place of the text, its library files as tabs, run with no note, Cmd-Z back in one step; the button's another; with twelve tabs, a notice and nothing changed); PRESETS (the browser opened with the program's title, a name typed and saved: the status and the header say so, EDITED after an edit; listed by name, Load asking over changes and then loading the text back, Save asking before it replaces, Delete asking; Escape); Escape and Tab |
| `gui.panel` | a `.gui` interface in the Panel, headless, to `gui.panel.code.png`, `gui.panel.png`, `.paper.png`, `.meters.png` and `.small.png`: in the CODE view under the editor, every item inside its area and none over another, the dials' own items hidden; the PANEL cell (the editor gone, the interface from the toolbar down, fitting; the processor and its state keep the choice); a large log knob's drag, double-click and middle (632 Hz, the host's text); a row of buttons, the switch, the menu (the host's, ticked; chosen), a slider's drag, the pad's two parameters; the envelope's corners; the tabs (one page shown, a click shows the other; a radio group to a screen reader); audio through the processor (the meter, the light, the number, the spectrum's peak at the sine's frequency and level); at the least size the wheel scrolls the interface, and turns a knob under it; a `#pragma panel` program in the PANEL view from the column's top |
| `lint.layers`, `lint.exports` | Source/dsp, Source/fsdsp and Source/fminus without JUCE or FunkGui, FMinus through its public header only; each bundle exports its entry points and nothing else |

A probe is one file in `Tools/probes/<layer>/` whose first line registers it (`// PROBE name=dsp.mytest timeout=60`)
and whose body uses spec rows (`P.near`, `P.le`, `P.ge`, `P.in`, `P.eq`): each states what must hold, with its
tolerance. `Tools/probes/common/Audio.h` has the signal helpers, and `Tools/probes/common/Rig.h` runs a program in an
Engine as the processor does.

## FMinus

[FMinus](https://github.com/Snipet/FMinus) (`~/audio/libraries/FMinus`) comes in through its public API,
`fminus/FMinus.h` (`fminus::Program`): `Source/fminus/Patch.cpp` compiles a program twice (once to read its module
and parameters, once with each float parameter bound as a stream and the plugin's calls appended), at `-O2`, flushing
subnormals.

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
                         GuiSpec (a .gui file read: its interface's tree, the controls' tapers and units);
                         Random (RANDOM: a program put together from the library's building blocks);
                         examples/*.fm, the EXAMPLES (compiled in by CMakeLists.txt as generated/Examples.h);
                         library/<n>-<category>/*.fm, the LIBRARY (as generated/Library.h)
Source/dsp/              the audio side, plain C++20 without JUCE: Engine (programs swapped without a click, the dials
                         glided into streams, the output guard, the levels, fields and scope the displays read),
                         Compiler (compiles off the audio thread), Controls, Sources (a program's files), Measure (a
                         program rendered offline and measured, for the chat)
Source/fsdsp/core/       FSynth's DSP core, vendored at the tag in SEED.tsv (scripts/sync-dsp-core.sh)
Source/plugin/           the JUCE AudioProcessor: P1 to P32, the program's files and the draft in the session, the
                         bypass;
                         EditorPort.h, what the editor reaches besides the parameters;
                         the chat: ChatAgent (the conversation, its turns and Claude's tools), ChatPrompt.cpp (Claude's
                         instructions: the panel, the library, a program built from it), ClaudeApi (the Messages API
                         over HTTPS, to Anthropic or the TestingPlayground server),
                         ChatStore (the key and the sign-in, encrypted), ChatPort.h (what the editor reaches of it);
                         PresetStore (the presets on this Mac)
Source/editor/           the FunkGui panel (Panel.cpp: the toolbar, the files' tabs and TextEditors, the problems, the
                         dials, the chat's card and the access dialog; its layout follows the window's size),
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
cmake/Product.cmake      the identity: name, kind (an effect), manufacturer and plugin codes, bundle id, namespace
```

## Make a new plugin

Start new plugins from FunkPluginTemplate (`../FunkPluginTemplate/scripts/plugin.py clone --name MyPlugin`, effect or
instrument). This project's own `scripts/plugin.py clone --name MyLab --code Mylb` copies the playground into a new
project with its own identity; `--type instrument`, `--starter` and `starter-diff` refuse here, because the starters
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
`~/Library/Application Support/TestingPlayground/`. Colours are tokens: `Source/editor/Theme.h` shows how to give
this plugin its own accent; errors use the theme's `signal` colour. `TSTN_UI_THEME=1` opens the Standalone on the
light theme without saving it, `TSTN_GPU_LOG=1` logs the GPU surface (the prefix is the product's,
`cmake/Product.cmake`).
