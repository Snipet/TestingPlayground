# Changelog

What changed in the two plugins, TestingPlayground (the effect) and TestingPlaygroundSynth (the synth), newest first:
one section for each day work was merged, and under it each pull request with what you can do since. The plugins are
at version 0.1.0 and have no tagged releases yet, so the days stand in for versions.

When a day changed the Claude Desktop extension, its section says the extension's new version. Open the new `.mcpb`
(or run the installer again) to update it: Claude Desktop replaces an extension only with a newer version.

## Unreleased

### The chat keeps its conversation through a stopped summary and an empty reply ([#40](https://github.com/Snipet/TestingPlayground/pull/40))

**Fixed**
- Pressing Stop while Claude was summarizing a long conversation (or the connection dropping then) no longer throws
  the conversation away: it goes on as it was. A chat already left that way opens again and works.
- A reply with nothing in it (no words, only spaces, or another model's reasoning alone) no longer breaks the chat.
  Every later message was refused until you started a new chat; now the reply shows as "(no answer)" and the
  conversation goes on. Spaces alone beside a tool call are left out too. A chat already left that way is mended when
  it opens.
- On macOS and Windows a message to Google Gemini went out malformed: the query in its address was moved into the
  request's body. The address now goes out as the plugin built it, query included.

### Your key and chats survive a full disk, and a message waits for its pictures

**Fixed**
- Saving the API key or a chat on a full disk (or past a quota) no longer replaces the saved one with an empty file
  while saying it was saved: the save says it failed, and the key or the chat kept before stays as it was.
- Chats copied from another computer or account (a copied Library, a backup restored elsewhere) no longer fill the chat
  list with rows titled "Chat" that open nothing. A chat in the list that cannot be read says so when you open it.
- A message sent while its pictures were still being read went without them, and they waited for the next message.
  The message now shows at once and goes with every picture once they are in; Stop still stops it.
- If the list of Claude's models could not be read when a session began (the API busy, the connection down, or Stop
  pressed at that moment), it was not asked for again until the plugin was opened again, so a model chosen from it
  went with the wrong length limit and without its thinking level. It is now asked for again at the next message.
- A message sent just as Claude Desktop began working on the plugin could start a reply beside it, after which the chat
  showed Send while Claude was still answering. The message now stays in the box until Claude Desktop is done, as it
  does whenever Claude Desktop works.

### The chat's buttons do what you meant, and your typing is never overwritten

**Fixed**
- A double-click on Send sent the message and then stopped Claude's reply at once (the button is Stop by the second
  click). The second click of a double-click now does nothing; a click of its own on Stop still stops.
- After a click on Send (or Stop), Space or Return stopped Claude's reply and other keys went to the host. The caret
  now goes back to the message box, so you can type the next message while Claude works.
- In a narrow window the Web pill covered the Send/Stop button, so a click on Stop switched web search instead. The
  bar now always fits: the pills narrow, and Fast and Web show their icons alone, before anything reaches Send.
- In the provider settings, typing another server's address left the dialog naming the preset's server (where the key
  is stored and sent, whether one is stored, Remove key). It now names the address you typed, once it is complete.
- The key field showed up to eleven characters of a key. It now shows only the provider's public prefix (`sk-ant-`,
  `sk-or-`, `AIza`, ...) and, for a long key, its last four characters; stars for the rest.
- A plugin window opened while the chat was folded (and the code window) redrew itself constantly while idle. It now
  draws only when something changes.
- Typing in a file at the moment Claude edited the same file could lose what you had just typed. Your keys now reach
  the plugin as you type them, and Claude's edit is made again over them.

## 2026-11-09

### Shaders on Windows, and Windows' tests counted ([#31](https://github.com/Snipet/TestingPlayground/pull/31))

**Fixed**
- On Windows the panel's shaders draw. FunkGui v0.26.1 takes the shaders Direct3D compiles: v0.26.0 refused each one
  that read the panel's size or the time.
- A shader may make a vector of one value as GLSL does, `vec3(x)`: the plugin hands it to the compiler as a cast,
  which Direct3D's takes as Metal's and Vulkan's do. On Windows such a shader did not compile.
- The profile of a program with parallel regions always says that only the rendering thread is sampled. With few
  samples the note came and went.
- On Windows the shader compiler no longer holds other open files of the plugin, or of the host, while it runs.
  A shader built at the same moment as another could lose its place in the cache and leave a temporary file there.

**Changed**
- A Windows run that fails now blocks a merge, as macOS's and Linux's do.

### Every pull request built and tested on macOS, Linux and Windows ([#30](https://github.com/Snipet/TestingPlayground/pull/30))

**Added**
- Each pull request, and each change to `main`, builds both plugins and runs every test on GitHub's machines: macOS
  (Apple silicon), Linux (x86-64) and Windows (x64). A failed run keeps its test log and the tests' pictures for 7
  days. The tests ran on Windows for the first time here; until the last of them pass, a Windows failure does not
  block a merge.

**Fixed**
- Cancelling a long measurement of the effect comes back at once. It used to look through the whole render first: a
  quarter of a second, or more on a slower machine.
- On Windows, a shader that does not compile says why, in the compiler's words, and the profile of a program with
  parallel regions takes its samples while the program's threads keep every core busy.
- The plugins build with older macOS SDKs and newer Windows SDKs.

## 2026-11-07

Claude Desktop extension: **0.17.0** (was 0.13.0).

### FunkGui v0.26.0 ([#28](https://github.com/Snipet/TestingPlayground/pull/28))

**Changed**
- The editor draws with less work: each shape goes to the GPU as one small record instead of six corners, about a
  fifth of the data each frame and less CPU, with the same pixels.

**Fixed**
- A test of the headless renderer could wait for ever on macOS when it fed the renderer its request through a pipe.

### Thinking levels for OpenRouter, OpenAI and Gemini ([#26](https://github.com/Snipet/TestingPlayground/pull/26))

**Added**
- The chat's **Thinking** menu now works with other providers. Each model offers its own levels (Off, Minimal, Low,
  Medium, High, Extra high, Max), with its default marked. OpenRouter's levels come from its model list; OpenAI's are
  for its reasoning models (o-series, GPT-5 and later); Gemini 3 takes levels and Gemini 2.5 a thinking budget. Models
  that don't reason, and servers on your machine (Ollama, LM Studio), show no menu.
- Until you choose a level, the model thinks as it does by default. Your choice is remembered for each provider and
  model.

**Changed**
- A model that refuses the level is asked again at once without it, and a note says so: the turn goes on. OpenRouter's
  errors now include the words of the model's own provider.

### A changelog, and a README that starts short ([#25](https://github.com/Snipet/TestingPlayground/pull/25))

**Added**
- This changelog. The README now opens with what the plugins do, building from a clone on macOS, Windows and Linux,
  and how to start; the full guide follows.

### Linux, capture marks, a target sound ([#24](https://github.com/Snipet/TestingPlayground/pull/24))

**Added**
- **Linux** (x86-64): both plugins as VST3 and Standalone, with the MCP server and the headless renderer. The editor
  draws with Vulkan. The chat's key goes to your desktop's keyring, or to an encrypted file where none runs; folders
  follow XDG; the synth's Standalone reads a G29 racing wheel through the kernel's driver. `scripts/package-linux.sh`
  makes a `.tar.gz` whose `install.sh` installs into `~/.vst3` and `~/.local`, with no root. Claude Code (or another
  MCP client) connects to the plugins there.
- **Capture marks**: mark a moment while Claude records what you play, and Claude reads the marks.
- **compare** measures against a target WAV of yours.
- `waveguide.fm` in the library: a Karplus-Strong string or tube.
- A cheat sheet of FMinus and DSP in Claude's guide.
- `level_wobble_db`, how much the loudness moves, in measurements, compare and checks.

**Fixed**
- The schematic: a new tab is kept when Claude writes at the same moment; its notes show beside "Not played"; the
  screamer's labels no longer cross its wires; far-apart parts are routed in groups; knobs, chips, the toolbar and the
  probe card are read by screen readers.
- The code editor: another module's constant used before that module is reported, as FMinus reports it; a local that
  shadows a constant is coloured as a local; long hover signatures are shortened.
- The CPU line names the real voice count; labels are cut between characters, never inside one; `unit=""` hides a
  field's unit; Cmd-S no longer stalls the editor; PROJECTS and Export open faster.

### MIDI learn, samples, factory projects, other model providers ([#23](https://github.com/Snipet/TestingPlayground/pull/23))

**Added**
- **MIDI learn** for the synth's dials: right-click a dial, **Learn MIDI CC**, move a knob on your controller. The
  mapping is kept with the session and with projects, shown on the dial, and can be forgotten.
- **Samples**: drop a WAV, AIFF or FLAC on the editor, and a program reads it as a table (`#pragma sample`). Samples are
  saved inside projects, and Claude can list and add them.
- **Claude Desktop's target**: one clear rule for which open plugin Claude Desktop works on (the one you choose with
  **Use with Claude Desktop**, else the only one open; with several and none chosen, Claude asks). Name an instance
  ("Bass"), and see when Claude Desktop is acting on it.
- **Factory projects**: projects that come with each plugin, read-only (a save makes your own copy). Placeholders for
  now.
- **The project browser**: groups by category (Factory first), favourites, search, tags and sorting.
- **Presets in turn**: arrows and the preset's name in the top bar, `[` and `]`, MIDI Program Change and the host's
  program list.
- **Claude saves**: Claude can save versions and presets, apply presets and open projects; it never overwrites your
  unsaved edits unless you ask.
- **Chats per project**, several per project, kept on this Mac only; exports never carry them.
- **Other model providers** for the chat: OpenAI-compatible servers (OpenAI, OpenRouter, Ollama or LM Studio on your
  machine) and Google Gemini, each with its own key and models.

### Double, 2-D arrays, longer renders, a headless renderer ([#22](https://github.com/Snipet/TestingPlayground/pull/22))

**Added**
- FMinus's `double`, 2-D arrays and field attributes (`ui::label` and `ui::unit` name a field's readouts and
  displays), in the editor and Claude's tools too.
- Renders that return numbers run up to 300 seconds, and a render can be stopped mid-way.
- Export a render's audio as WAV (24-bit or float) and chosen fields as CSV.
- **The headless renderer**: `TestingPlaygroundRender` and `TestingPlaygroundSynthRender` render and measure a program
  from the command line, with the plugin's own compiler: a WAV, a field log and the metrics.
- Claude imports text files into the program and exports to a folder, calls one function of the program by name to
  test it (run_function), and can profile where a program spends its time.

### Live panel edits, mono, Shared's hooks, menus in the editor's look ([#21](https://github.com/Snipet/TestingPlayground/pull/21))

**Added**
- A Run that changes only the panel (`panel.gui`) or a shader applies at once: the voices play on and the dials keep
  their values.
- More room on a panel: 16 shader inputs, 32 displays, and 32 dials of the panel's own (saved with projects and
  presets, not host parameters).
- Text in shaders (`text(...)`, `number(...)`), and `#include` of the built-in shader files and your own.
- `#pragma panel` number cells take `unit=` and `decimals=`.
- The synth: `#pragma mono` plays one voice, legato or retrigger, newest note first; the module Shared can run on every
  block, whether a voice sounds or not, and hear each note before its voice does.
- A program lights a G29's shift lights (`shiftLights`).

**Changed**
- Every menu is drawn in the editor's own colours and corners, with section headers, the current choice marked,
  shortcuts and type-ahead.

## 2026-11-06

Claude Desktop extension: **0.13.0** (was 0.11.0).

### The editor answers at once ([#20](https://github.com/Snipet/TestingPlayground/pull/20))

**Fixed**
- Hover, deselection and the Code/Panel switch show on the first frame again (they waited for the next mouse move).
- Switching between Code and Panel is instant (it took up to 60 ms with a large program and chat).

### Programs that stop safely, A/B, the code in its own window ([#19](https://github.com/Snipet/TestingPlayground/pull/19))

**Added**
- **A loop that never ends** no longer hangs your DAW: the call stops, the program fades out, the previous one plays
  on, and the error names the function.
- **Bounds checks** while you work: an index outside its array stops the program and names the file, line, array and
  index. The frozen copy plays without them, at full speed.
- FMinus's `#if`, `#ifdef` and `#define` (`DEBUG` is defined on RUN), and its warnings in the editor.
- The chat and the explorer are resizable (drag their edges; a double-click resets).
- The panel under the code (Cmd-J), and the code editor in a window of its own while the main window shows the panel.
- **Edit in a folder**: the program's files mirrored to a folder, so VS Code or any editor edits them, synced both
  ways.
- **A/B** between versions of a project: a diff per file, switching while it plays at matched loudness, Keep A or
  Keep B.

**Changed**
- Faster panels: an idle editor draws nothing, displays rest when nothing moves, automation and inputs update at full
  rate (they were held to 12 a second), shaders are cached, and the meters stop while the editor is hidden.

### Dials keep their units, editable checks, more measurements ([#18](https://github.com/Snipet/TestingPlayground/pull/18))

**Changed**
- A dial that keeps its name across a Run keeps its value in its units (clamped into a new range), not its knob's
  turn; a switch or menu keeps its choice's name when options are inserted. Projects and presets keep the name too.

**Added**
- Claude's checks can be listed and changed one by one.
- Measurements over a window in seconds; octave, third and semitone bands; integrated loudness (LUFS) and A-weighted
  level; compare at a second sample rate; dial sweeps.
- Claude edits several files as one undo step, and searches the program's files with a pattern.

## 2026-11-05

Claude Desktop extension: **0.11.0** (was 0.10.0).

### Room in Claude Desktop's tools ([#17](https://github.com/Snipet/TestingPlayground/pull/17))

**Changed**
- What Claude Desktop reads first, its tools and the start of its guide, is shorter (by 2.3 KB and about 1,100 tokens)
  and says the same, which leaves room for what came next.

### Windows ([#16](https://github.com/Snipet/TestingPlayground/pull/16))

**Added**
- **Windows** 10 (1803 or later) and 11, x64: both plugins as VST3 and Standalone, their Claude Desktop extensions, an
  installer (just for you, or for all users) and the tests. The editor draws with Direct3D 11, or Windows' own software
  renderer where no GPU starts. Projects live in `Documents\Funk`, and the chat's key is encrypted with Windows' own
  cryptography. Shortcuts say Ctrl where macOS says Cmd.

## 2026-11-02

Claude Desktop extension: **0.10.0** (was 0.7.0).

### The racing wheel ([#15](https://github.com/Snipet/TestingPlayground/pull/15))

**Added**
- A **Logitech G29** (wheel, pedals and shifter) as a MIDI input of the synth's Standalone, switched on in Settings.
  Programs read `steering`, `throttle`, `brake`, `clutch`, `gear` and `buttons`; in a DAW any MIDI source drives them
  (CC 16 to 24 and 48 to 51). The PS button plays a note.
- Claude drives the wheel in its renders, and recordings keep the wheel's moves.

### Language, panel and tool fixes ([#14](https://github.com/Snipet/TestingPlayground/pull/14))

**Added**
- FMinus: C's bit operators, `switch`, `enum`, constant tables, and `hash` and `randf` for white noise.
- Displays read the module Shared's fields; number readouts take `decimals=`, `unit=` and `scale=`; a program has up
  to 16 files (was 12).
- Claude reads part of a file or searches it, makes several edits at once, and never writes over text you changed
  after it read it.

**Changed**
- Measurements are steadier: spectra are averaged over everything held and are the same at every sample rate, and one
  rule finds clicks everywhere.
- `switch`, `case`, `default` and `enum` are keywords now, and `hash` and `randf` builtins: a saved program that uses
  one as a name must rename it (until then the previous program plays on).

**Fixed**
- A stolen synth voice, or one cut or dropped, now always gets its `stop()`, exactly once.

## 2026-10-31

Claude Desktop extension: **0.7.0** (was 0.5.0).

### The schematic view ([#13](https://github.com/Snipet/TestingPlayground/pull/13))

**Added**
- A **Schematic** view of a `.cir` netlist (Cmd-Shift-D): draw and edit the circuit, its parts, wires and values; the
  library's and the examples' netlists lay themselves out. The drawing lives in comment lines, so it never changes
  what plays.
- Knobs beside a circuit's pots and chips beside its switches, turned as the dials are, and **Play this circuit**.
- Live probes: up to four nets' voltages as scopes, with peak, RMS and mean.
- Claude edits the circuit as you do, and sees the drawing.

### EXAMPLES and RANDOM start afresh ([#11](https://github.com/Snipet/TestingPlayground/pull/11), [#12](https://github.com/Snipet/TestingPlayground/pull/12))

**Changed**
- Choosing an example (Init too) or pressing RANDOM starts afresh, as a new instance does: only its files, every dial
  at its default, no project open. It asks first when unsaved work would be lost; the chat stays.

## 2026-10-30

Claude Desktop extension: **0.5.0** (was 0.4.0).

### Analog circuits ([#10](https://github.com/Snipet/TestingPlayground/pull/10))

**Added**
- A `.cir` tab is a SPICE-style netlist (resistors, capacitors, inductors, pots, switches, diodes, OTAs, op amps,
  sources) that a program `#include`s and plays. Errors point at the netlist's own lines.
- In the library: `diodetone.cir`, `screamer.cir`, `otalp.cir` and `oversample.fm`. New examples: Tube screamer (the
  effect) and OTA filter (the synth).
- A circuit that leaves its simulation open (its oversampling, anti-aliasing and solver) plays a provisional one at
  once while a self-test in the background chooses; when it answers, the program swaps in as a RUN does.

## 2026-10-29

Claude Desktop extension: **0.4.0** (was 0.3.0).

### The LLVM IR view ([#9](https://github.com/Snipet/TestingPlayground/pull/9))

**Added**
- The compiled program as LLVM IR, in the explorer and a read-only tab: optimised, as first generated, and the frozen
  copy's.
- Go to a line (Ctrl-G) in every tab.

### Init ([#8](https://github.com/Snipet/TestingPlayground/pull/8))

**Added**
- **Init**, the program a new instance plays: the effect's passes the input through untouched (INPUT, OUTPUT, MIX),
  the synth's is a classic subtractive voice (one oscillator, a resonant low-pass filter, an ADSR).

**Fixed**
- In the synth's examples, a note on a stolen voice no longer skips its attack and clicks.

### Frozen dials in fast math ([#7](https://github.com/Snipet/TestingPlayground/pull/7))

**Changed**
- The frozen copy (the program compiled again with still dials as constants) plays in fast math again, which
  [#3](https://github.com/Snipet/TestingPlayground/pull/3) had turned off: the same sound, slightly faster on average.

### One installer for both plugins ([#6](https://github.com/Snipet/TestingPlayground/pull/6))

**Added**
- **TestingPlayground Installer**: one app for both products, each one's AU, VST3, app and Claude Desktop extension,
  just for you or for all users (macOS asks for the password, never the app), with Uninstall. Replaced copies go to
  the Trash. `scripts/package.sh --installer` makes its disk image.

### The GUI sandbox ([#5](https://github.com/Snipet/TestingPlayground/pull/5))

**Added**
- In Claude Desktop, Claude designs a panel (a `.gui` file) and sees it drawn exactly as the plugin draws it, with no
  plugin open; the design reaches your plugin only when you ask.

### The logo ([#4](https://github.com/Snipet/TestingPlayground/pull/4))

**Added**
- The logo, **Caret**: a wave written up to the editor's cursor, an effect and a synth variant, on the app and
  extension icons, the top bar and the README.

**Changed**
- The synth has an accent colour of its own.

### Settings for the apps, the frozen snowflake ([#3](https://github.com/Snipet/TestingPlayground/pull/3))

**Added**
- **Settings…** (Cmd-,) in the Standalone apps: a card in the editor's look for the audio output and input, sample
  rate, buffer size and MIDI inputs, with a test tone; it follows devices plugged in and out.
- A snowflake beside the CPU load while the frozen copy plays.
- Claude's pictures of the panel are drawn on the GPU, shaders included.

## 2026-10-28

Claude Desktop extension: **0.3.0** (was 0.2.0).

### Claude hears what plays ([#2](https://github.com/Snipet/TestingPlayground/pull/2))

**Added**
- Claude, in Claude Desktop and the plugin's chat, can **listen** to what plays now, **capture** what you play (kept in
  memory only) and replay it, **compare** two versions, presets or projects, and **trace** where a NaN, a click or a
  runaway starts and which lines cause it.
- Renders take a scenario (a phrase, automation, the wheels, the host's tempo) and trial edits on a copy.
- **Fix what I hear** in Claude Desktop's **+** menu.

### Your own key, a native window, a code editor ([#1](https://github.com/Snipet/TestingPlayground/pull/1))

**Added**
- The Standalone apps in a native macOS window, with Settings, File, Edit and Window in the menu bar.
- A code editor in the manner of VS Code: squiggles, completion, signature help, hover, Format Document, find and
  replace, go to definition, a minimap and a status line.
- `scripts/package.sh` makes a `.pkg` of both products; `scripts/install.sh` installs the apps too.

**Changed**
- No accounts: the chat uses your own API key only.
- Lighter on the CPU: the meters run only while someone looks at them, and the CPU figures are more accurate.

## 2026-10-15 to 2026-10-28: before pull requests

- **A new start.** TestingPlayground, a Visage plugin until then (the tag `legacy-visage`), was rebuilt on FunkGui with
  FSynth's DSP core.
- **FMinus inside.** FMinus, a JIT compiler for DSP, came in: first as a script on the reverb's output, then as the
  whole sound, a program typed into the plugin's editor and compiled on RUN, in several files with `#include`.
- **The chat.** Claude in the plugin writes and changes the program, measures what it puts out, looks at the panel and
  the sound, checks its own work and tunes the dials; fast mode, pictures, web search and a planner.
- **Dials and panels.** 32 dials in an analog panel's sections, dial styles and live displays, `.gui` interfaces with
  pages, knob looks and custom shaders; a library of building blocks, RANDOM, and wavetables without aliasing.
- **In a song.** The host's tempo, position and transport reach programs (`host.fm`), and dials that hold still are
  compiled in as constants (frozen dials).
- **Projects**: every save a version, presets inside each project, export as a source `.zip` or a compressed
  `.fmpack`, and import.
- **Claude Desktop**: an MCP server, packed as each plugin's Claude Desktop extension, whose own version starts at
  0.2.0.
- **Two plugins, one repository** (2026-10-28): the synth, TestingPlaygroundSynth (the Visage synth ported onto FSynth's
  DSP core, then rebuilt around FMinus as the effect was), joined this repository as a second product built from the
  same sources, its history with it.
