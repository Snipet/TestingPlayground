# TestingPlaygroundSynth: rules for a Claude session

A playground for writing synth voices while the plugin plays: an instrument plugin (AU, VST3, Standalone; macOS
arm64) on JUCE 8.0.4 whose FunkGui editor is a text field holding an FMinus program (a module Voice, one instance a
note), which the plugin compiles (RUN, Cmd-Return) and plays, its parameters becoming the dials. Made from
FunkPluginTemplate. Change the panel, the examples and the
plumbing freely; keep the rules below. README.md is the user's guide (writing a program, the editor, the probes); this
file is what a session must know (`scripts/plugin.py info` prints the identity).

<!-- origin -->
It was the legacy Visage synth (the tag `legacy-visage`; never move it) ported onto FSynth's DSP core, with an FMinus
script on its output, until the branch `fminus-editor` replaced the DSP and the panel. Its plugin code is its own: the
legacy code `Tstn`, which it shared with TestingPlayground (the original project), stays TestingPlayground's. The
effect playground (`../TestingPlayground`) has the same code with the effect's contract (`product::kIsInstrument`
picks): change both, and keep them the same but for the namespace, the examples and the docs.
<!-- /origin -->

## What goes where

| Path | What | Rule |
| --- | --- | --- |
| `cmake/Product.cmake` | identity: name, kind (an instrument), codes, bundle id, namespace (`testingplaygroundsynth`), environment prefix | `scripts/plugin.py` rewrites it; the plugin code stays unique among the user's plugins |
| `cmake/Deps.cmake` | the pins: JUCE, bgfx, FunkGui (v0.21.0: the TextEditor, resizable Panels, custom shaders, images, several editors in one host), FMinus (a commit) | move a pin's tag, commit and version together |
| `Source/fsdsp/core/**` | FSynth's DSP core, vendored (`SEED.tsv`) | do not edit casually: `scripts/sync-dsp-core.sh` refuses to overwrite edits |
| `Source/fminus/**` | `Patch` (a program compiled for the plugin: the contract, the appended calls, the instances and the module Shared's one, the taps: the main module's fields its displays read, the Host block: host.fm, built in, `Patch::setHost`); `PanelSpec` (its `#pragma panel` lines, read from the files it compiled: sections, dial styles and options, colours, displays); `GuiSpec` (a `.gui` file, the first of its files: the interface's tree, in place of the pragmas, and the tapers, units and scales it gives the dials; `fminus.gui`); `Random` (RANDOM: a program for this plugin's kind from the library's building blocks; `fminus.random` compiles and plays 160 seeds); `examples/*.fm`, the EXAMPLES (the first the default program; first line `// <Title>: ...`); `library/<n>-<category>/*.fm`, the LIBRARY (first line `// <file name>: what it is`, then its usage header; `fminus.library` measures each) | JUCE-free; FMinus only through `fminus/FMinus.h` (C++17, no LLVM), never its internals or LLVM's headers (`lint.layers`) |
| `Source/dsp/**` | `Engine` (programs swapped by crossfade, the dials as glided streams, voices for an instrument, the output guard, the meters the displays read: `Engine::Meters` and the scope's rings), `Compiler` (compiles off the audio thread, offers to the Engine, collects), `Controls.h` (slot mapping), `Events.h` (the MIDI as events, the host's `Transport`, the program's `HostBlock`), `Measure` (a program rendered offline in an Engine of its own and measured in words and in named numbers: the chat's measure_output; `Session`, one compile measured many times through `Engine::restart`), `Goals` (goals and checks on those numbers: set_checks), `Tune` (tune_dials' search), `SoundView` (view_sound's render and analysis) | JUCE-free C++20 |
| `Source/plugin/**` | the JUCE `AudioProcessor`: P1 to P32 (`ControlParameter`: the program's names and texts), one FunkGui port each, the program's files and the draft in the session, the bypass, the compiler's outcome polled on the message thread; `EditorPort.h`; the chat with Claude: `ChatAgent` (the conversation, the turn on its own thread, Claude's tools through `ChatHost`), `ChatPrompt.cpp` (the system prompt), `ClaudeApi` (the Messages API over HTTPS, to Anthropic or the TestingPlayground server), `ChatStore` (the key and the server's sign-in token, encrypted), `ChatImages` (pictures for a message: read with JUCE, Core Image for WebP and HEIC, scaled, thumbnailed), `SoundPicture` (view_sound's PNG, drawn with JUCE's software renderer on the agent's worker, and its thumbnail), `ChatPort.h` | reads parameters once per block, calls the Engine; copes with what a host may do (below); the chat's rules below |
| `Source/editor/**` | the FunkGui `Panel`, resizable (`layout()` places every item for the size; `kMinWidth` x `kMinHeight`): the top bar (the product, the project's pill, the Code / Panel switch, the load, the theme), the explorer's card at the left (`Explorer`: the program's files, the project open's presets, the projects, the library and the examples in sections that fold, its rows doing what the tabs, PROJECTS, LIBRARY and EXAMPLES do through the Panel's hooks; it folds away, its button then at the left of the program's card's header, `EditorPort::explorerOpen`; it is away in the PANEL view and when the program's card would keep less than `kProgramMinW`; `gui.explorer`), the program's card (its status and actions: Examples, Library, Random, Run; the files' tabs, a `funkgui::TextEditor` each with FMinus highlighting and error marks, the problems list, the dials (`DialLayout`: plain rows, or the panel's sections; `Looks`: the dial styles and the displays drawn; `DisplayItem`: a display's motion), or a `.gui` file's interface (`GuiLayout`: its flexbox layout; `GuiItems`: its controls in their looks, pad, envelope, keyboard, tabs and frames; `gui.panel`, `gui.styles`), its pages kept by title (`EditorPort::guiPages`), and the Code / Panel view (the session keeps it: `EditorPort::panelView`)); the chat's card, which folds into a rail (`EditorPort::chatOpen`); the chat's card (`ChatView`: the conversation, laid out and drawn as a chat app does; `ChatItems`: the message box and the chat's buttons; `ChatStyle.h`: the rounded look, its palette mixed from the theme's tokens) and the access dialog (account or API key); `ProjectBrowser`, the projects' card (the projects, the chosen one's versions and presets, Import and Export); `ExportCard`, EXPORT (a project's info, its two exports); `Picture.cpp`, view_panel's picture (a headless Panel on the message thread, for `ChatHost::chatPicture`); `Items.h`; `Theme.h`; `gpu/` the EditorHost factory (the window's size goes to the session) | reaches the processor only through its ports and `EditorPort` (the chat through `EditorPort::chat()`) |
| `Tools/probes/**` | probes on FunkGui's Harness: `fminus/` (the examples, the contract, the library, the panel's lines), `dsp/` (the Engine, its meters, the Host block), `plugin/`, `gui/`; `common/Rig.h` runs a program in an Engine | spec rows only, no golden files |

There are no starters here (`scripts/starters/` is the template's; the ones this project had predated the FMinus
editor and went with the old DSP): `plugin.py clone` copies this playground (its plugin code must be free across
`~/audio/plugins`; the probes' namespace alias follows the new name; the `<!-- origin -->` blocks are left out);
`--type effect`, `--starter` and `starter-diff` refuse. New plugins start from FunkPluginTemplate.

## Build and test

- `cmake --workflow --preset dev-verify`: configure, build (Standalone, AU, VST3, probes) and run every test in
  `build-dev`. Done means it exits 0 with no warning from our sources (they build with `-Werror`).
- `scripts/test.sh`: build and run the probes only (`-R <name>`, `-V` pass through to ctest). The editor rendered
  headless lands in `build-dev/probe-out/` (`gui.dump*.png`, `gui.chat*.png`: the empty chat, a conversation, the
  access dialog's key, sign-in and signed-in tabs, PAPER, the least size and a large one): look at the PNGs after any
  editor change.
- An example added or changed is checked by `fminus.examples` (compiles with no message, plays, the same bits in
  blocks of 64 and 333). A change to the contract (Patch.h) changes `fminus.contract`, the README's "Writing a
  program", the examples' comments and the placeholder text with it.
- Projects live in `~/Library/Audio/Presets/Funk/<name>/` (`Source/plugin/ProjectStore`, which the processor uses
  through `EditorPort`): a project is a program and its dials kept as versions (each save one, the newest 100 kept),
  with presets (dial settings, applied by name). Probes never touch the user's: under CTest the folder is the probe's
  sandbox (`<prefix>PREFS_DIR/presets`), and plugin.projects, gui.dump and gui.explorer point it at a temporary folder
  of their own (`projectstore::setDirectoryForTests`). The file format (`funk-playground-project`, version 1) is read
  by every later build: add fields, never change one's meaning. The presets from before projects (`.fmpreset`) are
  migrated, never deleted. `Source/plugin/ProjectExport` writes a project as a source .zip (`funk-playground-source`)
  or a .fmpack (a 20-byte header, then the project's JSON as xz through macOS's libcompression, zlib elsewhere) and
  reads both back; the same rule holds for both formats. plugin.export checks them.
- `cmake --workflow --preset release-build`: Release with LTO in `build-release`. `scripts/install.sh` copies the AU
  and VST3 into `~/Library/Audio/Plug-Ins`; only the user (or the lead) runs it, never an agent.
- The presets build at `-j6` (the machine is shared). No network: JUCE, bgfx and shaderc come from `~/audio/.deps`
  (read-only, never write there) and FunkGui is cloned from `~/audio/libraries/FunkGui` at the pinned commit. FMinus is
  cloned from `~/audio/libraries/FMinus` at the pinned commit (`FP_FMINUS_SHA`; `-DFETCHCONTENT_SOURCE_DIR_FMINUS=`
  builds its working tree instead), and its LLVM, Bison and Flex are read from `~/audio/libraries/FMinus/.deps`, the
  installs FMinus's own builds made: never build them from here (FMinus's "system" LLVM mode, which `Deps.cmake` sets,
  never does).

## Layer and real-time rules

- `Source/dsp`, `Source/fsdsp` and `Source/fminus` include nothing from JUCE, FunkGui, `plugin/` or `editor/` (the test
  `lint.layers` checks; the DSP library has no JUCE include path). `Source/plugin/EditorPort.h` stays JUCE-free too: the
  editor includes it. FMinus comes in through its public header, `fminus/FMinus.h`, only: its internals and LLVM's
  headers appear nowhere in `Source/`.
- Each AU and VST3 exports JUCE's entry points and nothing else (CMakeLists.txt's exported-symbols lists; the test
  `lint.exports` checks the built bundles). LLVM's weak symbols, exported, would be merged with every other plugin's
  that exports them, and the plugins would share LLVM's state.
- FMinus compiles off the audio thread only (`Compiler`: its thread, or `compileNow` on the message thread or a host's
  state call). A program's instances, and its module `Shared`'s one (init'd before them, end'd after), are created,
  reset (`init`) and destroyed (`end`) off the audio thread too; the audio thread only calls the appended nonblocking
  functions (process, start, stop) and sets parameters. Never call a program function through another type than its
  declaration's: the appended lines (Patch.cpp) are what make the types right, so a new call needs its own line.
- The Engine's handovers are two atomic pointers (`pending_`, `retired_`): offer, take at a block's start once the last
  swap is over, hand back. The audio thread never deletes a Patch. `Engine::setControls` and `process` allocate, lock
  and log nothing; `prepare()` (audio stopped) is the only place that sizes buffers.
- Nothing steps: a program swap fades the old program's voices over 20 ms (the held keys start again on the new
  one), a steal fades over 2 ms, a real dial glides over 20 ms as a stream, the bypass crossfades over 20 ms. The
  output does not depend on how the host splits its blocks (the examples are checked at 64 and 333, a release mid-block
  included), except where a dial moves (once a block): the voices run in calls cut on the absolute 16-sample grid and
  are freed only on it (Engine::kGrid), so keep both.
- Frozen dials (Patch.h, Engine.h, Compiler.h): the Compiler's freezer thread compiles the playing program again with
  its dials as FMinus frozen constants once `Engine::dials` says they have held still for 3 s (from the recipe the
  compile kept, `Patch::Recipe`), and offers it (`Engine::offerFrozen`); the audio thread plays it only if it is of
  the program that plays at exactly the dials' values, through the same instances (`Patch::useFrozen`), and drops it
  the block a dial moves; copies go back through four retired slots for `collect`. The copy compiles in FMinus's
  Math::Fast (the user's choice, 2026-10-27: faster, the same sound), so it is not the same bits as the program's
  Math::Vector: its last bits move, and while it plays the output may depend on where the host's blocks end. Never
  add fast-math flags or anything else that changes more than a math function's last bits. `dsp.freeze` (its own
  program: the same samples, finite and the difference 80 dB under the signal; the examples, and in LONG 64 RANDOM
  programs for ten seconds: the same spectrum, every octave band, level and peak within 0.1 dB, since an FM voice's
  phase drifts an ulp at a time) and plugin.process FREEZE check it; a probe that compares two processors' bits must
  not run long enough to freeze (3 s of still dials), or turns the freezer off (`Compiler::setFreezing`).
- The displays read what the audio thread publishes, without a lock: after each block the Engine measures the
  output and reads the displays' fields (`Patch::tap`, a field of the newest sounding voice's instance) into
  `Engine::Meters`, through an `fsdsp::Seqlock`, and writes the scope's ring of atomics; the editor reads them each
  frame (`Panel::tick`). `Meters::program` names the program the fields are of (`Patch::program`, the compile's
  request id): the panel shows them only on that program's displays, since the audio thread takes a new program at
  its next block. Nothing there allocates; `dsp.meters` checks the values.
- The output guard (Engine::guard) stays: whatever a program computes, what leaves the plugin is finite and within
  +-4. Hosts and speakers come first.
- The processor copes with what a host may do: the bypass crossfades between the Engine's output and silence and the
  Engine keeps running (on the note-offs only, so a note let go during the bypass releases); a block before any `prepareToPlay` passes the input through, sanitised; a
  host's NaN keeps the last good value (and its text is a dash); a state value that is not a number loads as 0.5.
  `plugin.process` and `plugin.state` check each.
- The Host block (host.fm: `kHostFile` in `Patch.cpp`, the one file the plugin provides; `Patch::hostSource` puts it in
  the system prompt): the processor reads the playhead once a block into `Engine::setTransport` (none: the Engine's
  own clock at 120 BPM, not playing) and MIDI's CC 1, CC 64, pitch bend and channel pressure into events at their
  offsets; the keyboard's wheels come through `EditorPort::playWheel` and the processor's lock-free ring of UI events.
  The Engine fills the block before every call part (`syncHost`, the position at the part's first sample) and holds
  the sustain pedal's notes itself (`Voice::pedal`). A new field: kHostFile, `HostBlock`, `HostLayout` and `setHost`
  together; `dsp.host` checks.
- Flags: `-ffp-contract=off`, never `-ffast-math`. Product constants come from the generated `Product.h`, never from
  `JucePlugin_*`. The editor never uses JUCE's parameter attachments: widgets write through FunkGui's
  `GestureController` to the ports (`funkgui::JuceParamPort`).

## The chat (Claude)

- **The network.** The chat is the only code that reaches the network at run time, over HTTPS to two hosts
  (`ClaudeApi.cpp`), and only when the user sends a message, signs in or signs out: `api.anthropic.com` with the
  user's own key (the Messages API, streamed; `GET /v1/models` once per key), or the TestingPlayground server
  (`claude::serverHost()`: `kServerHost`, or `<prefix>SERVER_URL` when it is https:// or http:// on this Mac, as for
  `wrangler dev` at http://localhost:8787) once the user has signed in: `POST /auth/login` and `/auth/logout`, the same
  `/v1/messages` and `/v1/models` with `Authorization: Bearer <token>` instead of `x-api-key`, and `GET /account`
  after a turn (the footer's credit). The server's own repo is TestingPlaygroundBackend (its README has
  the API). The build never reaches the network. Probes never do: they give the agent `fake::Transport`
  (`Tools/probes/plugin/FakeClaude.h`, a scripted API and server) and a key folder of their own
  (`chatstore::setDirectoryForTests`; under CTest the default folder is the test's sandbox, `<prefix>PREFS_DIR/chat`).
  A session never makes a live call to Anthropic or the server (it is billed to the user) unless the user asks for
  one.
- **The key and the sign-in** live only in their encrypted files (`ChatStore.h` says the format and what it protects
  against): `anthropic-key.bin` and `account-token.bin`; the password typed to sign in is sent once and kept nowhere.
  Never log any of them, print them, put them in the session, a probe's output, a test fixture or a commit; the panel
  masks the key and the password and speaks only their lengths. A probe's are `fake::fakeKey()`, `fake::fakeToken()`
  and `fake::fakePassword()`.
- **The account.** Both a key and a sign-in can be stored; the chat uses the one set up last (`chat.json`'s "use",
  `ChatPort::useAccount`). Through the server a request is the same as to Anthropic (the server only adds
  `metadata.user_id`); its 401 always means the sign-in is over (the token is forgotten and the chat asks the user to
  sign in again); its 402 (credit used up), 403 (disabled), 400 (a model it has no price for) and 502 (Anthropic
  refused its key) carry messages written for the user (`claude::serverError`), shown as they are and never retried.
  Signing in and out runs on a thread of its own (`ChatAgent::auth_`), never on the message thread. `plugin.chat`'s
  ACCOUNT and `gui.chat`'s ACCOUNT check each.
- **The API.** Requests follow the Claude API reference (the `claude-api` skill): the `anthropic-version` header
  (`claude::kApiVersion`), adaptive thinking with `output_config.effort`, no sampling parameters, the system prompt
  cached for an hour and the automatic breakpoint, tools with `eager_input_streaming` whose input is parsed strictly
  here (`INVALID_JSON` results), `"fallbacks": "default"` with its beta on the models that take it, fast mode
  (`"speed": "fast"`, chosen with the Fast pill, on Claude Opus 5.5 without a beta and on Opus 5 and 4.8 with
  `fast-mode-2026-02-01`; such a request goes without `fallbacks`, a 429 or 529 twice runs the turn on at standard
  speed, a refusal about the speed turns it off), and an append-only history (preserved thinking: nothing sent before
  is changed, except as the fallback rule says). Check the reference before changing a request; `plugin.chat` checks
  the shape.
- **Threads.** ChatPort calls run on the message thread; a turn runs on the agent's worker, which reaches the plugin
  only through `ChatHost` (the draft under `programMutex_`, RUN through the Compiler, a dial queued for `poll()`). The
  audio thread never sees the chat.
- **Large work.** `max_tokens` is the model's own limit (from /v1/models, at most 128K); server-side compaction (beta
  `compact-2026-01-12`, trigger 150K input tokens, instructions that keep the design and leave out code) on the
  models that take it, the compaction block kept and what came before it dropped: the history then starts at the
  block, in its assistant message (never at the message before it, which mid-turn is tool results the API refuses
  without their calls; `repairHistory`, also run on a loaded session), and the files go again with whatever follows,
  a message or the turn's next tool results; a file's text goes in the plugin's state only when Claude has not seen
  it (`seen_`); a broken stream is retried with what it showed taken back; 80 requests a turn. A reply cut off at
  `max_tokens` (thinking counts) goes back as it came and the turn goes on with a line that tells Claude; a second
  in a row ends the turn. The prompt says what a reply holds; at xhigh and max a note with the user's message says it
  again (`limitNote`, kept in the history as sent). `plugin.chat` checks each.
- **The system prompt and the tools** bind the thinking blocks of a conversation (preserved thinking): the session
  saves their hash (`promptPrint`), and a session saved with another loses its thinking when it loads
  (`dropThinking`). Changing either is fine; editing a message already sent is not.
- **measure_output** renders the editor's files offline (`dsp/Measure.h`, a fresh compile in an Engine of its own,
  never the one that plays) and reports in words; `plugin.chatexample` checks its numbers on programs whose answer is
  known. The same pass records named numbers (`measure::metrics()`: each a name, a unit, a line of what it is and a
  default tolerance; the system prompt's CHECKING YOUR WORK lists them from the table, so a metric added there is
  taught at once); a request's `want` runs only the renders and the analysis they need. A `Session` compiles once
  and renders as often as asked, each render after `Engine::restart` (offline only: prepare() and `Patch::reset`), so
  it gives what a fresh compile gives; `dsp.tune` checks that, and the numbers.
- **view_sound** (`dsp/SoundView.h`, `plugin/SoundPicture.h`): the render analysed (the waveform's extent and level
  per column, a Blackman-Harris spectrogram on a log axis, the held part's average spectrum, a close-up) and drawn as
  a 1120 x 800 PNG on the worker thread with JUCE; it goes in the tool's result like view_panel's (the session keeps
  a line in its place) and its 480 px thumbnail on the tool's entry, which the chat draws under the row (the session
  keeps the thumbnail).
- **set_checks** (`dsp/Goals.h`): checks are goals (a metric with min, max or a target and a tolerance) with a label
  and a render; `ChatAgent::evaluateChecks` measures them all (one compile, one render for checks that share one) on
  the editor's files and the plugin's dials after every run_program that compiles (the results end its result) and
  when a reply ends a turn in which a tool changed something (`kChangingTools`, a result that is no error); a failure
  there adds the results as a user text message and the turn goes on, at most `kMaxCheckNudges` (2) times a turn. One `ChatEntry::Kind::checks` card a turn, brought up to date in place; its
  Clear calls `ChatPort::clearChecks`. The session keeps `checks_` in set_checks's own shape. Never let a check
  nudge without a cap: each nudge is a billed request.
- **The planner** (`setPlanner`, chat.json "planner"; ChatAgent.h "A planner"): `planTurn` sends a message's
  history to the planning model through `forAnotherModel` (no thinking blocks, which are bound to the builder's model
  and prompt; no compaction or server-tool blocks; citations dropped) with `tool_choice` none and the tools defined
  (the history's tool calls stay valid), a brief after the user's words; the plan streams into a
  `ChatEntry::Kind::plan` entry and is appended to the user's message before the builder's first request (never to
  a message already sent). `ask_planner` sends the history up to the reply that asks. A failing planner leaves the
  turn to the builder with a note. `plugin.chat` PLANNER checks the requests' shapes.
- **tune_dials** (`dsp/Tune.h`): Nelder-Mead over the named float dials' normalised ranges, from the dials as they
  are and a Halton spread, restarted when it settles with a goal unmet, until every goal is met, the budget (150, at
  most 400 renders) is spent or the stop button cancels; deterministic. The best values go on the dials through
  `chatSetDial`.
- **The system prompt** (`ChatPrompt.cpp`) teaches Claude FMinus, the contract, how to build a large program, the
  `.gui` file's language (THE INTERFACE: GuiSpec.h; the `#pragma panel` lines only as the older way), the library (every file's header, from Library.h) and the examples, and
  shows a program built from the library for this plugin's kind (`ChatAgent::promptExample`: the program, its
  `panel.gui` and the library files it includes), which `plugin.chatexample` compiles and plays: a change to the contract (Patch.h), the
  panel's lines, the language's rules, a library file's header or the examples changes it too, and the example must
  keep compiling with no note.
- **Pictures** (`attach`/`detach`, `ChatImages.h`): read on the auth thread, kept as sent (the file's own PNG or
  JPEG bytes when they fit, else encoded again) with a thumbnail (RGBA8, and encoded for the session); sent as base64
  image blocks before the message's text; six a message, twenty in the history. The editor draws thumbnails through
  FunkGui's image registry (`funkgui::images`, v0.20.0): `Panel::syncPictures` registers each picture's id while
  `chat_` holds it, counted across Panels (Picture.cpp makes one), so the last to let go removes it.
- **Web search** (`setWeb`, chat.json "web"): the API's server tools web_search and web_fetch (20260209) go in the
  request only on a turn with the user's own key (`turnWeb_`; the server does not charge for searches), and a turn
  whose tools differ from the history's drops its thinking (`historyWeb_`, in the session as "web"). Their blocks
  (`server_tool_use`, the `*_tool_result`s with their encrypted parts, text with `citations`) go back as they came
  (`MessageBuilder` keeps them); `pause_turn` sends the history again with nothing after it. The sources listed under
  an answer are the citations' pages (the docs ask for them).
- **ask_user** pauses the turn: the reply's other tools run and their results wait in the history; the question's
  entry stays open (`ChatState::asking`) until `ChatPort::answer` (the labels clicked) or `send` (the user's own words,
  after a result that says so), and either starts the next turn with the question's result first in the user
  message (`missingResults`, `deliver`: tool results before any text, as the API asks). One ask a reply; the session
  keeps an open question ("asking"). The view draws it as a card (`ChatView`: ShapeKind::option, ActionKind::option and
  submit).
- **add_library_file** puts a library file in the editor as a tab (never over a file of that name) and returns its
  whole text, so Claude has seen it (`seen_`); the program's `#include` is Claude's to write.

## Recipes

- **An example**: a file `Source/fminus/examples/<n>-<name>.fm` whose first line is `// <Title>: what it is`; it
  appears in the EXAMPLES menu in file-name order (the first is the default program). Run `fminus.examples`.
- **A library file**: `Source/fminus/library/<n>-<category>/<name>.fm`, its first line `// <name>.fm: what it is`
  (the LIBRARY menu shows it), then `//` and a usage header (the comment lines it starts with, which Claude's
  instructions quote); names that no other library file uses; no allocation outside methods init and end call; safe
  before its set or allocate. Measure it in `fminus.library` (a harness there runs its modules over buffers), and run
  `plugin.chatexample` (the headers are in the prompt).
- **A new call into programs** (a note's own pressure, say; what the host or the player is doing belongs in the Host
  block instead): add a `Call` row in `Patch.cpp` (the method, whether
  it is required, how the plugin declares it, the appended line), a pointer in `Patch::Compiled` looked up in
  `compile()`, a method on `Patch` the Engine calls; document it in Patch.h and the README.
- **A view**: add an `Item` in `Source/editor/Panel.cpp` (a FunkGui widget, or your own drawing on the
  `funkgui::Canvas`, under a tag of its own after `ui::kMarkTag` when a probe needs to find it) and place it in the
  constructor; then check `gui.dump`'s PNGs.
- **A .gui element** (a control, a display, a container): its `GuiKind` in `PanelSpec.h` and its word, arguments,
  properties and notes in `GuiSpec.cpp`, its natural size in `GuiLayout.cpp`, its item and drawing in `GuiItems` (a
  display's in `DisplayItem`), placed by `Panel::layoutGuiItems`; teach it in the prompt's THE INTERFACE
  (`ChatPrompt.cpp`) and GuiSpec.h's header; run `fminus.gui`, `gui.panel` and `gui.styles` (look at their PNGs) and `plugin.chatexample`. Shaders
  (`fminus/Shaders.h`: built on the compiler's thread with the bundled shaderc; `GuiShaderItem`, registered by
  `Panel::registerShaders`) have `gui.shaders`; the build copies shaderc into each bundle (`CMakeLists.txt`) and CTest
  gives the probes its path (`<prefix>SHADERC`). A property every
  element of a kind could take goes through `class` and `defaults` without more: they hand on properties, not kinds.
- **A dial style or a display kind** of the `#pragma panel` lines: its enum in `PanelSpec.h` (`DialStyle`,
  `DisplayKind`) and its word in `PanelSpec.cpp` (with a note where it does not suit), its size in `DialLayout`, its
  drawing in `Looks`, a display's motion in `DisplayItem` (a dial's in Panel.cpp's `DialItem`); run `fminus.panel`,
  `gui.dump` (look at `gui.dump.displays*.png`) and `plugin.chatexample`.
- **A RANDOM part**: a choice in `Random.cpp` whose lines name only what it declares itself, its name in `parts` and
  `allParts()`; `fminus.random` must reach it (raise `kSeeds` if no seed does) and play it cleanly. A library file
  RANDOM uses keeps its header's API, or Random.cpp changes with it.
- **A probe**: a file `Tools/probes/<layer>/<name>.cpp` whose first line is `// PROBE name=<layer>.<name> timeout=<s>`
  and whose body is `PROBE(<layer>, <name>) { ...; return P.finish(); }`. Use spec rows (`P.near`, `P.le`, `P.ge`,
  `P.in`, `P.eq`); helpers are in `Tools/probes/common/Audio.h` and `Tools/probes/common/Rig.h`.

## Git

Work on a branch; commit messages say what changed and why. Never commit build directories (`build-*/` is ignored).
Text about this repository's own history goes in `<!-- origin -->` blocks (README.md, CLAUDE.md): a clone has no
such history, and `plugin.py clone` leaves them out.
