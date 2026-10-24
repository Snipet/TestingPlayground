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
- **Dials.** The module's first eight parameters, in the order they are declared, are the dials, and the host's
  parameters P1 to P8 (a host shows "1 gain", and the value in the program's units). A `float` glides when its dial
  turns: it is read sample by sample, `gain[i]`, as a stream of 20 ms ramps, and held still otherwise. An `int` or a
  `bool` is read once a call (`mode[0]`, or any index). `fminus::range` sets the dial's span (a float without one
  spans 0 to 1, an int 0 to 10) and `fminus::default` where it starts and where a double-click returns it. A run
  that brings a parameter the slot did not have moves that dial to its default; one with the same name keeps it.
- **The audio thread.** process may not allocate: FMinus refuses a program whose process could reach `calloc`,
  `malloc` or `free`, and names the line. Allocate in init (a delay line: `calloc`, zeroed) and free in end, both
  off the audio thread; or use a fixed array, `float line[48000];`, a field like any other.
- **Modules inside modules.** The main module may hold others (`Svf l;`, `Osc osc[4];`): their methods are its
  helpers, and their parameters start at their defaults (they have no dials).
- **Several files.** The editor has a tab for each file, the program's first (`program.fm`). The program includes
  another by its name, `#include "filters.fm"`, as C does: the file is read where the line is (a module there is a
  type after it), and once only, as if every file began with `#pragma once`, so two files may both include a third.
  Its errors name it ("filters.fm:3:12: error: ..."). FMinus has no other directive: no macros, no `#if`.
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
from calloc in init, freed in end) and Drive (tanh saturation). To add one, add a file there whose first line is
`// <Title>: what it is`.

## The editor

- **RUN** compiles the text and plays it once it compiles, crossfaded over 20 ms; **Cmd-Return** does the same from
  the keyboard. Compiling takes some tens of milliseconds, off the audio thread and off the editor's.
- **EXAMPLES** replaces the program's text with an example (Cmd-Z gives yours back) and runs it; the other files
  stay.
- **The files' tabs**, over the text: a click shows a file; a double-click edits its name in place (Return keeps it,
  Escape cancels; letters, digits, `_`, `-` and `.`, and no two the same); **+** adds a file (up to eight), its name
  ready to type; a tab's **x** closes its file, asking first when it has text. The program's tab cannot be closed. A
  tab whose file the last run's errors name has a mark before its name. Each file keeps its own caret, scroll and undo.
- **The status** says whether the program plays, whether the text has changed since it was run, and how many errors
  the last run had; a program that does not compile leaves the last one that did playing. At the right: the audio
  thread's load (the busiest recent block's share of its time) and the output guard's alerts.
- **The text** of the file shown is FunkGui's TextEditor: FMinus's words highlighted, line numbers, undo (Cmd-Z,
  Cmd-Shift-Z), the clipboard, word and line moves (Alt and Cmd with the arrows), Tab to indent (four spaces), Escape to
  leave the text (the panel's keys then move the focus over the dials). It has the caret when the editor opens.
- **Problems.** The last run's messages are listed under the text, with their file, line and column; a click on one
  shows its file and puts the caret there, and each line with an error has a mark in the gutter.
- **The dials** are the program's parameters (above); drag, scroll, or use the keys once Tab has focused one.
- **The session** keeps the program that plays (all its files), the files as you left them (run or not), the tab
  shown, and the dials. A session whose program no longer compiles (an older FMinus, say) leaves the first example
  playing, its files in the editor. A session saved before files loads as a program of one file.
- **The window** resizes in both directions, from 1000 x 600 (logical px, before ZOOM) up: the editor and the chat's
  conversation take the height, the chat about a third of the width. The session keeps the size.
- THEME (header) and ZOOM (footer) are machine-wide preferences.

## The chat

The column at the right is a conversation with Claude, through the [Claude API](https://docs.claude.com) with your
own key. Claude sees the plugin's files, whether they play, the last compile's messages and the dials (sent with each
message when they changed), and it can act on them: read, write, edit, rename and delete the files (the editor's tabs
follow, and Cmd-Z in a file undoes its change), run the program (RUN; the compile's messages come back to it), set
the dials, and read the CPU load and the output guard's alerts. Ask it for an effect ("a tape delay with wow and
flutter"), a change ("make the filter resonate more"), an explanation ("what does this program do?"), or help with
an error or a click. It knows FMinus and the plugin's contract (its instructions are in
`Source/plugin/ChatPrompt.cpp`).

- **KEY** asks for your Anthropic API key (from console.anthropic.com, Settings, API keys): paste it (Cmd-V) and
  SAVE. A message sent before a key is stored opens the same dialog and goes once the key is saved. REMOVE forgets
  the key.
- **The model** (the button at the top: a menu of the models your key can use that think adaptively) and **THINKING**
  (low to max: how much Claude thinks before it answers, and how hard it works) are machine-wide, shared by both
  playground plugins. Higher levels and larger models answer better and cost more.
- **The message box**: Return sends, Shift-Return breaks the line. While Claude works, the line above the box says
  what it is doing and SEND is **STOP**, which ends its turn at once (what it did stays). **NEW** forgets the
  conversation (it asks first). After each reply the line shows its tokens: in (and how many were read from the
  cache) and out.
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
| `fminus.contract` | what a program needs, and what the plugin says when it does not fit: no module `Effect`, no `process`, a `process` of another type (refused, never called), one that could allocate, a syntax error with its line; eight dials and a note for the ninth parameter; a held module's parameters at their defaults; init gets the rate; several files: an included function plays, the main module in an included file, a file two files include read once, an included file's error with its name and include stack, a file not found, the plugin's message for a wrong `process` after an `#include` |
| `dsp.swap` | the Engine: no program passes the input; the first program fades in from it, a second crossfades from the first over 20 ms with no step beyond the signal's own, bit for bit outside the fades, and the old one is handed back; a real dial glides over 20 ms sample by sample, an integer one steps at the block; a NaN, an infinity and 10 leave as 0, 0 and 4, with their alerts |
| `plugin.process` | the processor as a host drives it: P1 to P8 named and worded as the program's, host writes reach the program (within 0.05 dB), a host's NaN ignored (its text a dash), a block of 0 samples and a block larger than announced change nothing, the bypass passes the input bit for bit and its edges do not click, a block before prepareToPlay passes the input with NaN and inf as 0 |
| `plugin.state` | the session round-trips (a program of two files, the draft's three files and its tab, the dials, the output bit for bit); a run moves a renamed parameter's dial to its default and keeps a kept one's; a session whose program does not compile leaves the last one playing; one without a program plays the first example; one saved before files loads as one file; one with too many files or bad names loads tidied; a NaN dial loads as 0.5 |
| `plugin.chatstore` | the key on disk, in a folder of the probe's own: a key's shape; saved, the file is the format above with no 12 characters of the key in it, 0600 in a 0700 folder, and loads back; saved twice it differs (fresh salt and IV); one byte changed anywhere, or the file cut short, and it does not load; forgotten, it is gone; the model and level round-trip, a damaged chat.json loads as none |
| `plugin.chat` | the chat's agent against a scripted API (no network): the event stream however it is cut, a message rebuilt (thinking and its signature, text, a tool call's streamed input, the usage); no key, no request; a turn that writes a program, runs it, sets its dial and answers; each request's shape (model, adaptive thinking, effort, the cached system prompt, the tools, fallbacks, the API version) and the history append-only; another model and level; a tool input that is not JSON; a retry after 529; a 401; a refusal; a fallback; STOP; the session; NEW |
| `gui.chat` | the chat column and resizing, headless, to `gui.chat.png`, `.key.png`, `.paper.png`, `.small.png` and `.large.png`: the hint; the key dialog (opened by a message with no key, Escape, modal, a key refused, pasted, never shown or spoken, SAVE sends the waiting message, REMOVE); Return and Shift-Return; a turn shown (thinking, what Claude did, the answer, a code block, the usage), its files as tabs, the program playing, Cmd-Z undoing Claude's change; the model menu and the levels; STOP; NEW; the least size and a large one, every item inside; the session keeps the size |
| `gui.dump` | the panel headless to `gui.dump.png`, `.paper.png`, `.error.png`, `.edited.png` and `.files.png`: settles, every glyph, the draft in the editor and a dial for each parameter; typing, Cmd-Return, a syntax error (status, problem line, gutter mark, the click that jumps to it), the EXAMPLES menu and its undo; the files' tabs (+, renaming in place, a taken name refused, Escape, typing into a second file, an `#include` of it that runs, its error's problem and marks, the click that shows its file, closing with and without asking); a dial's drag and double-click, Escape and Tab |
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
Source/fminus/           Patch: a program compiled for the plugin (the contract, the calls, the instances), JUCE-free;
                         examples/*.fm, the EXAMPLES (compiled in by CMakeLists.txt as generated/Examples.h)
Source/dsp/              the audio side, plain C++20 without JUCE: Engine (programs swapped without a click, the dials
                         glided into streams, the output guard), Compiler (compiles off the audio thread), Controls,
                         Sources (a program's files)
Source/fsdsp/core/       FSynth's DSP core, vendored at the tag in SEED.tsv (scripts/sync-dsp-core.sh)
Source/plugin/           the JUCE AudioProcessor: P1 to P8, the program's files and the draft in the session, the
                         bypass;
                         EditorPort.h, what the editor reaches besides the parameters;
                         the chat: ChatAgent (the conversation, its turns and Claude's tools), ChatPrompt.cpp (Claude's
                         instructions), ClaudeApi (the Messages API over HTTPS), ChatStore (the key, encrypted),
                         ChatPort.h (what the editor reaches of it)
Source/editor/           the FunkGui panel (Panel.cpp: the toolbar, the files' tabs and TextEditors, the problems, the
                         dials, the chat's column and the key dialog; its layout follows the window's size),
                         ChatItems (the conversation and the message box), Items.h, Theme.h;
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
