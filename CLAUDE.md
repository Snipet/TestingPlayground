# The playgrounds: rules for a Claude session

Two playgrounds for writing DSP while the plugin runs, built from one source tree: the effect, TestingPlayground
(`products/effect.cmake`), and the synth, TestingPlaygroundSynth (`products/synth.cmake`). Each is a plugin (AU, VST3,
Standalone; macOS arm64) on JUCE 8.0.4 whose FunkGui editor is a text field holding an FMinus program (the effect's
main module is `Effect`, one instance; the synth's `Voice`, one instance a note), which the plugin compiles (RUN,
Cmd-Return) and plays, its parameters becoming the dials. Made from FunkPluginTemplate. Change the panel, the examples
and the plumbing freely; keep the rules below. README.md is the user's guide (writing a program, the editor, the
probes); this file is what a session must know (`scripts/plugin.py info` prints the identities).

<!-- origin -->
The effect was an FDN reverb (the legacy `legacy-visage` FDN, ported) with an FMinus script on its output, and the
synth the legacy Visage synth ported onto FSynth's DSP core, until the branch `fminus-editor` of each replaced the DSP
and the panel. They were two repositories, the same code but for the namespace, the examples and the docs, until this
one took both: the synth's history is merged in ("History: the synth playground's commits"), and its files came as the
second product. The tag `legacy-visage` here is the effect's; never move it. The synth's plugin code is its own: the
legacy code `Tstn`, which it once shared with the effect (the original project), stays the effect's. Shared fixes from
FunkPluginTemplate are carried over by hand (`Source/plugin`, `Source/editor`, the plugin and gui probes,
`scripts/plugin.py`), keeping this project's own changes.
<!-- /origin -->

## The products

- **One source tree, compiled once per product.** A product is `products/<key>.cmake` (its `PRODUCT_*` values), its
  examples (`Source/fminus/examples/<key>/`) and its Claude Desktop extension's icon (`Source/mcp/icon-<key>.png`);
  everything else is shared. CMakeLists.txt's `fp_add_product` builds each against its own generated `Product.h`
  (`<build>/generated/<key>/`), and the code picks the kind's behaviour from `product::kIsInstrument` (and the other
  constants there). A change to shared code is a change to both products: check both.
- **Nothing outside `products/` names a product** (the docs aside): the C++ namespace is `playground` in both
  (`FP_NAMESPACE`; the probes' alias is `pg`), code reads `product::kName` and its siblings, CMake the `PRODUCT_*`
  variables, scripts `products/<key>.cmake`. The names may change before a release (`scripts/plugin.py rename`).
- **What users have must not change** with the code: per product the plugin's name, bundle id, manufacturer and plugin
  codes (the AU type and subtype, the VST3 class ids follow from them), version, environment prefix (`TSTN_`,
  `TSYN_`), FunkGui's Objective-C prefix (`Tstn`, `Tsyn`: they must differ, since a host loads both into one process)
  and preferences folder, the projects' folder (`~/Library/Audio/Presets/Funk/<name>/`), the MCP sockets' folder
  (`Funk-<name>-mcp`) and the plugin's state format; and the chat's key folder both share
  (`~/Library/Application Support/Funk/Playground/`). CMake refuses two products with one name, code, bundle id or
  prefix.
- **The products' kinds differ where the README says**: the effect's `Effect` processes the input in place, has an
  `in` to display and passes the input through its bypass; the synth's `Voice` plays notes (MIDI, the panel's
  keyboard, the wheels, the sustain pedal), sixteen at once, and is silent in its bypass. Everything else, the editor,
  the chat, the projects, the MCP server, the library, is the same in both.

## What goes where

| Path | What | Rule |
| --- | --- | --- |
| `products/<key>.cmake` | a product's identity: name, kind (`effect.cmake` an effect, `synth.cmake` an instrument), version, company, codes, bundle id, environment and Objective-C prefixes | `scripts/plugin.py` reads and rewrites them; the plugin code stays unique among the user's plugins; nothing else names a product |
| `cmake/Deps.cmake` | the pins: JUCE, bgfx, FunkGui (v0.22.0: the TextEditor and its code-editor hooks, resizable Panels, custom shaders, images, several editors in one host), FMinus (a commit) | move a pin's tag, commit and version together |
| `cmake/Probes.cmake` | each product's probe executable and tests (`fp_add_probes`), the lints once (`fp_add_lints`) | test names `<key>:<layer>.<name>`, labels `probe`, `<layer>`, `<key>` |
| `Source/fsdsp/core/**` | FSynth's DSP core, vendored (`SEED.tsv`) | do not edit casually: `scripts/sync-dsp-core.sh` refuses to overwrite edits |
| `Source/fminus/**` | `Patch` (a program compiled for the plugin: the contract, the appended calls, the instances and the module Shared's one, the taps: the main module's fields its displays read, the Host block: host.fm, built in, `Patch::setHost`); `PanelSpec` (its `#pragma panel` lines, read from the files it compiled: sections, dial styles and options, colours, displays); `GuiSpec` (a `.gui` file, the first of its files: the interface's tree, in place of the pragmas, and the tapers, units and scales it gives the dials; `fminus.gui`); `Random` (RANDOM: a program for the product's kind from the library's building blocks; `fminus.random` compiles and plays 160 seeds); `Lang` (FMinus's language service, reusable by the chat's tools: its tokens, the program's symbols across its files, host.fm and the library, completion, signature help, hover, definitions, occurrences; `LangFormat.cpp`, the formatter, which checks that it changes no token; `fminus.lang`); `examples/<key>/*.fm`, a product's EXAMPLES (the first the default program; first line `// <Title>: ...`); `library/<n>-<category>/*.fm`, the LIBRARY, every product's (first line `// <file name>: what it is`, then its usage header; `fminus.library` measures each) | JUCE-free; FMinus only through `fminus/FMinus.h` (C++17, no LLVM), never its internals or LLVM's headers (`lint.layers`) |
| `Source/dsp/**` | `Engine` (programs swapped by crossfade, the dials as glided streams, voices for an instrument, the output guard, the meters the displays read: `Engine::Meters` and the scope's rings), `Compiler` (compiles off the audio thread, offers to the Engine, collects), `Controls.h` (slot mapping), `Events.h` (the host's `Transport`, the program's `HostBlock`, an instrument's MIDI as events), `Measure` (a program rendered offline in an Engine of its own and measured in words and in named numbers: the chat's measure_output; `Session`, one compile measured many times through `Engine::restart`), `Goals` (goals and checks on those numbers: set_checks), `Tune` (tune_dials' search), `SoundView` (view_sound's render and analysis) | JUCE-free C++20 |
| `Source/plugin/**` | the JUCE `AudioProcessor`: P1 to P32 (`ControlParameter`: the program's names and texts), one FunkGui port each, the program's files and the draft in the session, the bypass, the compiler's outcome polled on the message thread; `EditorPort.h`; the chat with Claude: `ChatAgent` (the conversation, the turn on its own thread, Claude's tools through `ChatHost`), `ChatPrompt.cpp` (the system prompt), `ClaudeApi` (the Messages API over HTTPS, to Anthropic), `ChatStore` (the key, encrypted), `ChatImages` (pictures for a message: read with JUCE, Core Image for WebP and HEIC, scaled, thumbnailed; an MCP result's made to fit), `SoundPicture` (view_sound's PNG, drawn with JUCE's software renderer on the agent's worker, and its thumbnail), `ChatPort.h`; `McpLink` (the instance's socket for the MCP server: info, tools, guide, call and cancel; a call through `ChatAgent::mcpCall` on its worker, the quick ones on its poll thread); `StandaloneMenu` (the Standalone's menu bar as data: `gui.window`) | reads parameters once per block, calls the Engine; copes with what a host may do (below); the chat's rules below |
| `Source/plugin/standalone/**` | the Standalone app, compiled into each product's Standalone target only (`JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP`): `App.cpp` (its `JUCEApplication`, window and content, with `juce::StandalonePluginHolder` for the devices and the state, in JUCE's own settings file and keys: `filterState`, `audioSetup`, `windowX`/`windowY`; the effect's muted-input bar; the menu bar's commands), `NativeWindow.mm` (Objective-C++, ARC: the window's transparent title bar over full-size content, a view of ours in the title bar that keeps the buttons where the editor wants them, the drag and the double-click, the menu bar) | the AU and the VST3 never include it; the editor learns of the window only through `EditorPort::windowButtons`, `placeWindowButtons` and `dragWindow` (`Processor::setWindowHooks`; none in a host); a key the editor uses never becomes a menu shortcut (the editor gets every key first) |
| `Source/editor/**` | the FunkGui `Panel`, resizable (`layout()` places every item for the size; `kMinWidth` x `kMinHeight`): the top bar (the product, the project's pill, the Code / Panel switch, the load, the theme; in the Standalone, room at its left for the window's buttons, and its empty parts drag the window: `EditorPort::windowButtons`, `gui.window`), the explorer's card at the left (`Explorer`: the program's files, the project open's presets, the projects, the library and the examples in sections that fold, its rows doing what the tabs, PROJECTS, LIBRARY and EXAMPLES do through the Panel's hooks; it folds away, its button then at the left of the program's card's header, `EditorPort::explorerOpen`; it is away in the PANEL view and when the program's card would keep less than `kProgramMinW`; `gui.explorer`), the program's card (its status and actions: Examples, Library, Random, Run; the files' tabs, a `funkgui::TextEditor` each made a code editor by `CodeAssist` (FMinus's highlighter, the problems' squiggles and marks from RUN and from the check while typing, completion, signature help, hover cards, Format Document and Format on Run, the find bar, the line commands, go to definition, the status line; the completion list, the signature and the hover card drawn over everything; `gui.code`), the problems list, the dials (`DialLayout`: plain rows, or the panel's sections; `Looks`: the dial styles and the displays drawn; `DisplayItem`: a display's motion), or a `.gui` file's interface (`GuiLayout`: its flexbox layout; `GuiItems`: its controls in their looks, pad, envelope, keyboard (an instrument's), tabs and frames; `gui.panel`, `gui.styles`), its pages kept by title (`EditorPort::guiPages`), and the Code / Panel view (the session keeps it: `EditorPort::panelView`)); the chat's card, which folds into a rail (`EditorPort::chatOpen`); the chat's card (`ChatView`: the conversation, laid out and drawn as a chat app does; `ChatItems`: the message box and the chat's buttons; `ChatStyle.h`: the rounded look, its palette mixed from the theme's tokens) and the key dialog; `ProjectBrowser`, the projects' card (the projects, the chosen one's versions and presets, Import and Export); `ExportCard`, EXPORT (a project's info, its two exports); `Picture.cpp`, view_panel's picture (a headless Panel on the message thread, for `ChatHost::chatPicture`); `Items.h`; `Theme.h`; `gpu/` the EditorHost factory (the window's size goes to the session) | reaches the processor only through its ports and `EditorPort` (the chat through `EditorPort::chat()`) |
| `Source/mcp/**` | `<Name>MCP` (TestingPlaygroundMCP, TestingPlaygroundSynthMCP), the MCP server Claude Desktop starts (`Main.cpp`: JSON-RPC 2.0 on stdio, one `poll()` loop over stdin and the instances' sockets; list_instances, playground_guide and the chat's tools, routed to the instance named or used last; the prompts), `McpProtocol.h` (the private folder, the sockets, the line protocol, shared with `plugin/McpLink`) and `McpGuide.h` (the guide cut into parts, shared too); the build packs it with the product's `icon-<key>.png` as `<Name>.mcpb` (a Claude Desktop extension) | juce_core and POSIX only (and the generated `Library.h`): no plugin code; stdout is the protocol's, written by the loop alone |
| `Tools/probes/**` | probes on FunkGui's Harness, each run for every product: `fminus/` (the examples, the contract, the library, the panel's lines), `dsp/` (the Engine, its meters, the Host block), `plugin/`, `gui/`; `common/Rig.h` runs a program in an Engine | spec rows only, no golden files; where the kinds differ, a probe branches on `product::kIsInstrument` |

There are no starters here (`scripts/starters/` is the template's, and new plugins start from FunkPluginTemplate).
`plugin.py add` makes another product of this tree from one of these (its kind, a copy of its examples and its icon;
its plugin code must be free across `~/audio/plugins` and here); `plugin.py rename` gives a product a new identity in
place (its `products/<key>.cmake`, and its names in the docs).

## Build and test

- `cmake --workflow --preset dev-verify`: configure, build (both products' Standalone, AU, VST3, MCP server and
  probes) and run every test in `build-dev`: each of the 28 probes once for each product (`effect:fminus.examples`,
  `synth:fminus.examples`, ...), and the lints once. Done means it exits 0 with no warning from our sources (they build
  with `-Werror`). While working on one product, `cmake --workflow --preset effect-verify` (or `synth-verify`) builds
  and tests it alone (the build preset and target `<key>`, the test preset `dev-<key>`: label `<key>`); the gate is
  still dev-verify.
- `scripts/test.sh [effect|synth]`: build and run the probes only, of both products or the one named (`-R <name>`,
  `-L <label>`, `-V` pass through to ctest). Run the probes through ctest only: outside CTest a probe's stores are the
  user's own folders. The editor rendered headless lands in `build-dev/probe-out/<key>/` (`gui.dump*.png`,
  `gui.code*.png`: the code editor's completion, signature, hover, problems, find bar and PAPER;
  `gui.chat*.png`: the empty chat, a conversation, the key dialog with a key pasted and with one stored, PAPER, the
  least size and a large one): look at both products' PNGs after any editor change.
- An example added or changed is checked by its product's `fminus.examples` (compiles with no message, plays, the same
  bits in blocks of 64 and 333; the synth's voices end). A change to the contract (Patch.h) changes `fminus.contract`,
  the README's "Writing a program", the examples' comments and the placeholder text with it, for the kind it touches.
- Projects live in `~/Library/Audio/Presets/Funk/<name>/`, each product's own (`Source/plugin/ProjectStore`, which the
  processor uses through `EditorPort`): a project is a program and its dials kept as versions (each save one, the
  newest 100 kept), with presets (dial settings, applied by name). Probes never touch the user's: under CTest the
  folder is the probe's sandbox (`<prefix>PREFS_DIR/presets`, in `build-dev/sandbox/<key>/<probe>/`), and
  plugin.projects, gui.dump and gui.explorer point it at a temporary folder of their own
  (`projectstore::setDirectoryForTests`; its name starts with the product's prefix, as both products' probes run at
  once). The file format (`funk-playground-project`, version 1) is read by every later build: add fields, never change
  one's meaning; a project names its product and kind, and the other kind's is refused. The presets from before
  projects (`.fmpreset`) are migrated, never deleted. `Source/plugin/ProjectExport` writes a project as a source .zip
  (`funk-playground-source`) or a .fmpack (a 20-byte header, then the project's JSON as xz through macOS's
  libcompression, zlib elsewhere) and reads both back; the same rule holds for both formats. plugin.export checks them.
- `cmake --workflow --preset release-build`: Release with LTO in `build-release`, both products. `scripts/install.sh`
  copies both products' AU and VST3 into `~/Library/Audio/Plug-Ins` and their Standalone apps into `/Applications`
  (`~/Applications` when that is not writable; `scripts/install.sh synth`: one; `--dry-run` says what it would copy);
  only the user (or the lead) runs it, never an agent. `scripts/package.sh` makes the installer from the release build,
  `build-release/dist/<Name>-<version>.pkg`: a choice a product, each of its AU, VST3, app (system folders) and
  extension (`/Library/Application Support/<Company>/<Name>/`, which the installer's last page says to double-click: a
  package never opens Claude Desktop). Unsigned unless given identities (`--sign`, `--sign-installer`, `--notarize`, or
  `DEVELOPER_ID_APPLICATION`, `DEVELOPER_ID_INSTALLER`, `NOTARY_PROFILE`): it signs copies under `dist/stage/`, every
  Mach-O inside out with the hardened runtime and a timestamp, each app with `scripts/entitlements/<kind>.plist`
  (`allow-unsigned-executable-memory`: FMinus's JIT maps without `MAP_JIT`, so `allow-jit` alone gets it killed). An
  agent may build and inspect the .pkg (`pkgutil --expand`, `--payload-files`), never install it (no `installer`, no
  copy into `/Applications`) nor sign it.
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
- Each AU and VST3 exports JUCE's entry points and nothing else (CMakeLists.txt's exported-symbols lists, one pair a
  product; the test `lint.exports` checks every product's built bundles). LLVM's weak symbols, exported, would be
  merged with every other plugin's that exports them, and the plugins would share LLVM's state; the two products,
  whose C++ is one namespace, would share their own code's too.
- FMinus compiles off the audio thread only (`Compiler`: its thread, or `compileNow` on the message thread or a host's
  state call; the editor's check while typing too, `Compiler::check`: `Patch::check`, FMinus's first pass over the
  text and the main module's methods, nothing made to play and nothing offered). A program's instances, and its module `Shared`'s one (init'd before them, end'd after), are created,
  reset (`init`) and destroyed (`end`) off the audio thread too; the audio thread only calls the appended nonblocking
  functions (process, start, stop) and sets parameters. Never call a program function through another type than its
  declaration's: the appended lines (Patch.cpp) are what make the types right, so a new call needs its own line.
- The Engine's handovers are two atomic pointers (`pending_`, `retired_`): offer, take at a block's start once the last
  swap is over, hand back. The audio thread never deletes a Patch. `Engine::setControls` and `process` allocate, lock
  and log nothing; `prepare()` (audio stopped) is the only place that sizes buffers.
- Nothing steps: a program swap crossfades (the effect) or fades the old program's voices over 20 ms (the synth: the
  held keys start again on the new one), a steal fades over 2 ms, a real dial glides over 20 ms as a stream, the
  bypass crossfades over 20 ms. The output does not depend on how the host splits its blocks (the examples are checked
  at 64 and 333, the synth's with a release mid-block), except where a dial moves (once a block): the synth's voices
  run in calls cut on the absolute 16-sample grid and are freed only on it (Engine::kGrid), so keep both.
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
  input (the effect's) and the output and reads the displays' fields (`Patch::tap`, a field of the effect's instance,
  or of the synth's newest sounding voice) into `Engine::Meters`, through an `fsdsp::Seqlock`, and writes the scope's
  rings of atomics; the editor reads them each frame (`Panel::tick`). It measures the levels and writes the rings only
  while someone reads them (`Engine::readers`: each Panel adds itself while it lives, `EditorPort::addMeterReader`; a
  tool that listens adds itself, `Processor::addMeterReader`, and removes itself after): with no reader the levels
  read 0 and the rings stand still, the fields go on, and a headless Panel with no other reader (view_panel's picture)
  waits for two blocks measured. `Meters::program` names the program the fields
  are of (`Patch::program`, the compile's request id): the panel shows them only on that program's displays, since
  the audio thread takes a new program at its next block. Nothing there allocates; `dsp.meters` checks the values.
- **What a tool that listens reads** (the MCP's listen, `plugin/Live.cpp`): `Meters` also carries, every block and
  whoever reads, the host block the programs read (the transport, an instrument's wheels and pedal), the samples
  counted, an instrument's voices (sounding, released, held by the pedal, fading), the keys held, the player's
  note-ons (`Engine::heard`, in the event loops only: the held keys a new program starts are none), the steals (set on
  the steal paths in `noteOn`) and the last voice dropped for going non-finite (its key and sample); counts run on from
  the Engine's making, so readers take differences. `Engine::blocks` is a ring of atomics of each block's cost and
  context (`BlockStat`, written after the load measurement). The processor keeps the host's bypass, its largest block
  and its block count in atomics (`hostBlock`). Scalar stores only: no lock, no allocation. Readers never call
  `Engine::takeAlerts` (it clears the latch `poll` reads): they read the latched `chatAlerts`.
- The output guard (Engine::guard) stays: whatever a program computes, what leaves the plugin is finite and within
  +-4. Hosts and speakers come first.
- The processor copes with what a host may do: the bypass crossfades between the Engine's output and the input (the
  effect) or silence (the synth), and the Engine keeps running (the synth's on the note-offs only, so a note let go
  during the bypass releases); a block before any `prepareToPlay` passes the input through, sanitised; a host's NaN
  keeps the last good value (and its text is a dash); a state value that is not a number loads as 0.5.
  `plugin.process` and `plugin.state` check each.
- The Host block (host.fm: `kHostFile` in `Patch.cpp`, the one file the plugin provides; `Patch::hostSource` puts it in
  the system prompt): the processor reads the playhead once a block into `Engine::setTransport` (none: the Engine's
  own clock at 120 BPM, not playing); the Engine fills the block before every call part (`syncHost`, the position at
  the part's first sample). The synth also reads MIDI's CC 1, CC 64, pitch bend and channel pressure into events at
  their offsets, its keyboard's wheels come through `EditorPort::playWheel` and the processor's lock-free ring of UI
  events, and the Engine holds the sustain pedal's notes itself (`Voice::pedal`); the effect's wheels stay at rest. A
  new field: kHostFile, `HostBlock`, `HostLayout` and `setHost` together; `dsp.host` checks.
- Flush-to-zero: the programs compile for it (`Denormals::Flush`), and the plugin runs them in it on every thread:
  `Engine::process` opens an `fsdsp::ScopedFtz` (the audio thread has it already; offline renders, Measure and the
  chat's tools, would otherwise pay FMinus's wrapper two FPCR writes a call), and so do `Patch::reset` and its end
  (a program's init and end). The dials reach an instance through slots looked up at compile (`Patch::setValue`,
  `setStream`: inline stores, as FMinus's own setters write them), and the Engine writes them only when they change
  (`Engine::feedGen_`): keep both when the dials' path changes.
- Flags: `-ffp-contract=off`, never `-ffast-math`. Product constants come from the generated `Product.h`, never from
  `JucePlugin_*`. The editor never uses JUCE's parameter attachments: widgets write through FunkGui's
  `GestureController` to the ports (`funkgui::JuceParamPort`).

## The code editor

- **`fminus/Lang.h` is the language, as an editor sees it**: plain C++ over the files' text, no FMinus call. Its
  tables (keywords, type words, the builtins with their signatures, the attributes, `#pragma panel`'s words) follow
  FMinus at its pin and PanelSpec: a pin move that adds a keyword or a builtin adds it there, with `fminus.lang` rows.
- **The formatter never changes what a program means**: it re-indents, re-spaces and breaks lines (before a `{` that
  opens a block, around a `}` after code), never joins them, and it compares the tokens it wrote with the text's and
  gives the text back unchanged when one differs. `fminus.lang` renders the examples and RANDOM programs formatted:
  the same bits.
- **The check while typing** runs on the compiler's thread after a RUN waiting, never swaps the program that plays,
  and its messages land only in tabs whose text is what was checked (a later edit waits for the next check); RUN's do
  the same. RUN's messages are kept (`CodeAssist::setRunDiagnostics`): a check of the files RUN compiled shows them
  again, as they hold what only RUN finds, and RUN cancels a check due. The chat's message box is a TextEditor too and
  gets none of this.

## The chat (Claude)

- **The network.** The chat is the only code that reaches the network at run time, over HTTPS to one host
  (`ClaudeApi.cpp`), and only when the user sends a message: `api.anthropic.com` with the user's own key (the Messages
  API, streamed; `GET /v1/models` once per key). The build never reaches the network. Probes never do: they give the
  agent `fake::Transport` (`Tools/probes/plugin/FakeClaude.h`, a scripted API) and a key folder of their own
  (`chatstore::setDirectoryForTests`; under CTest the default folder is the test's sandbox, `<prefix>PREFS_DIR/chat`).
  A session never makes a live call to Anthropic (it is billed to the user) unless the user asks for one.
- **The key** lives only in its encrypted file (`ChatStore.h` says the format and what it protects against):
  `anthropic-key.bin`, in the folder both products share. Never log it, print it, put it in the session, a probe's
  output, a test fixture or a commit; the panel masks it and speaks only its length. A probe's is `fake::fakeKey()`.
  With no key stored the chat asks for one, and for nothing else.
<!-- origin -->
- **No accounts.** Until 2026-10-28 the chat could also sign in to a TestingPlayground server and go through it
  instead of a key. That is gone (the repository is open source): the agent deletes what it kept when it starts
  (`chatstore::forgetOldSignIn`: `account-token.bin` and `chat.json`'s "account" and "use"), and a session from then
  loads as any other. `plugin.chatstore`'s OLD and `plugin.chat`'s OLD check it.
<!-- /origin -->
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
  taught at once); a request's `want` runs only the renders and the analysis they need. Its CPU figures are the
  rendering thread's own time (`CLOCK_THREAD_CPUTIME_ID`, read at a render's few boundaries, never a block's), in an
  Engine as the audio thread runs it with no editor open: an instrument's `cpu_percent` over the held part, and
  `cpu16_percent` measured with sixteen notes held (the least of two short renders), not extrapolated; of the
  program's own code with its dials held still, every call flat (a dial that moves costs more: the Filter example
  about 2.7x while its cutoff glides; the frozen copy that plays once they have held still a while usually costs
  less), and the CPU line says so. A `Session` compiles once and renders as often as asked, each render after
  `Engine::restart` (offline only: prepare() and `Patch::reset`), so it gives what a fresh compile gives; `dsp.tune`
  checks that, and the numbers.
- **view_sound** (`dsp/SoundView.h`, `plugin/SoundPicture.h`): the render (the effect's with an input: a sweep, noise,
  an impulse, a sine or silence; the synth's the notes held, then released) analysed (the waveform's extent and level
  per column, a Blackman-Harris spectrogram on a log axis, the held part's average spectrum, a close-up) and drawn as
  a 1120 x 800 PNG on the worker thread with JUCE; it goes in the tool's result like view_panel's (the session keeps
  a line in its place) and its 480 px thumbnail on the tool's entry, which the chat draws under the row (the session
  keeps the thumbnail).
- **set_checks** (`dsp/Goals.h`): checks are goals (a metric with min, max or a target and a tolerance) with a label
  and a render; `ChatAgent::evaluateChecks` measures them all (one compile, one render for checks that share one) on
  the editor's files and the plugin's dials after every run_program that compiles (the results end its result) and
  when a reply ends a turn in which a tool changed something (`kChangingTools`, a result that is no error); a failure
  there adds the results as a user text message and the turn goes on, at most `kMaxCheckNudges` (2) times a turn. One
  `ChatEntry::Kind::checks` card a turn, brought up to date in place; its Clear calls `ChatPort::clearChecks`. The
  session keeps `checks_` in set_checks's own shape. Never let a check nudge without a cap: each nudge is a billed
  request.
- **The planner** (`setPlanner`, chat.json "planner"; ChatAgent.h "A planner"): `planTurn` sends a message's
  history to the planning model through `forAnotherModel` (no thinking blocks, which are bound to the builder's model
  and prompt; no compaction or server-tool blocks; citations dropped) with `tool_choice` none and the tools defined
  (the history's tool calls stay valid), a brief after the user's words; the plan streams into a
  `ChatEntry::Kind::plan` entry and is appended to the user's message before the builder's first request (never to
  a message already sent). `ask_planner` sends the history up to the reply that asks. A failing planner leaves the
  turn to the builder with a note. `plugin.chat` PLANNER checks the requests' shapes.
- **MCP** (`ChatAgent::mcpTools`, `mcpGuide`, `mcpGuidePart`, `mcpLive`, `mcpCall`, `mcpCancel`; `plugin/McpLink`;
  `Source/mcp`): Claude Desktop drives the plugin with the chat's own tools, so a tool changed or added reaches it
  with no change elsewhere; ask_user and ask_planner stay the chat's. A new tool gets a line in `kFaces`
  (ChatAgent.cpp: its title in words, read-only, destructive, and the guide part its notes go in, "start" unless
  said), and its description's first sentence (or first paragraph, when it has several) must stand alone: that is
  all MCP's copy keeps (tools.json sits in every Desktop conversation), the rest goes in the guide's THE TOOLS: WHAT
  THEIR SHORT DESCRIPTIONS LEAVE OUT, in that part. Each product has its own server, extension and socket folder.
  The server is juce_core only; it finds the instances by their sockets (`<n>.sock`, the lowest number free: the
  instance's name, "Effect 2" or "Synth 2", shown in the chat card while connected) and falls back on the tools.json
  and guide.md the newest left. It is for someone who never sees a tool: its instructions keep tools and ids from the
  user, each tool has a title in words, an instance is matched by name, number, track or project words, and an
  unclear one makes Claude ask in the user's words; the prompts (Make a sound, Improve the sound, Explain the sound,
  Fix what I hear) name sounds and tracks, never tools, are text only, carry the plugin's get_status when the
  instance is clear, and one whose tools the plugin lacks is not listed. `plugin.mcp` checks both sides. The
  extensions have their own version (`FP_MCP_VERSION`, CMakeLists.txt, both products'): raise it with every change
  Claude Desktop should pick up, or it keeps the old one.
- **MCP threads.** The server is one loop (`poll()` over stdin and the sockets): a request never waits for an
  instance, its answer is a continuation run when the reply comes; one writer owns stdout; several calls may be in
  flight. A tool call has a deadline (`kCallMs`, 200 s, under Claude Desktop's 240 s; `<prefix>MCP_DEADLINE_MS` in the
  probes): past it the client is answered ("still running") and the instance asked to stop it; a client's
  `notifications/cancelled` becomes a `cancel` line too; a late reply is dropped (never two answers to one id), and a
  slow instance is never dropped, only one whose socket closes. The link (`McpLink`) answers info, tools, guide,
  cancel and the live calls on its poll thread, and runs every other call on its one worker, in order (the chat's
  `busy_` allows one at a time; a call is refused while a chat turn runs); replies are whole lines under one mutex.
  Neither end ever waits to write: the server's and the link's sockets are non-blocking, what a socket cannot take
  yet waits in that connection's outbox (sent on `POLLOUT`), and each end keeps reading meanwhile, so a large request
  and a large reply crossing each other (more than the 8 KB a Unix socket holds on macOS) cannot leave both waiting
  for the other to read, and the link always stops at once. Keep it so: no blocking `send` on either side. Mixed
  versions: a `guide` request naming no part (a 0.2.0 server's) gets the whole guide; the server always names one.
  A cancel stops the call running (`ChatAgent::mcpCancel`, by the call's number: it sets `cancel_`, which tune's
  progress reads, and a tune cancelled so applies nothing) or drops one waiting; a connection closing cancels its
  calls, and the link going (the plugin closing) cancels and waits for the one running.
- **A live tool** (`ChatAgent::mcpLive`, one line a tool: get_status now; listen, and capture's status and stop,
  reserved) runs on the link's poll thread while a call may be running on its worker, without `busy_`, never refused.
  Such a tool's handler is called concurrently with a running call (a tune, a render, a compile) from another thread,
  so it reads only atomics, seqlock copies (`Engine::Meters`) or data under its own mutex (ChatHost's locked getters
  count: the draft under `programMutex_`, the Compiler's status, the dials under `dialMutex_`, as get_status reads
  them); it never touches the program (a Patch, a Session), the editor, the chat's history or `busy_`, never waits
  for the message thread or the worker, and returns in milliseconds. A tool that cannot keep that is not live.
- **listen and get_status's `since`** (`plugin/Live.cpp`, live: their reads keep the contract above). listen is
  the live line reserved above: a call that takes seconds (`ChatAgent::mcpLiveWaits`), so the link runs each on a
  thread of its own (`McpLink::listen`, four at once at most; a cancel or the connection closing stops it, the link
  going cancels and joins them), never on the poll thread; it raises the meters' reader count while it listens
  (`ChatHost::chatListening`, lowered on every exit) and polls `ChatHost::chatLive` about 25 times a second; a cancel
  that comes before it starts is kept (`mcpStopped_`, every one, not only the last). get_status ends with a token of the
  processor's journal (`plugin/Journal.h`: about 256 changes, drags merged, under its own mutex; what Claude Desktop
  does itself is not kept, `ChatAgent::mcpWorking`; each token handed out is a new one), and `since` lists what changed
  after it, the files as diffs (`dsp/TextDiff.h`) against their texts at that token (`mcpMarks_`, the newest 16
  tokens', under `liveMutex_`). One server may serve several conversations and its calls name none: what is kept
  for Claude Desktop is kept by token, never as one record of what "it" saw, which one conversation's look would move
  for all. The two Claudes keep their own records: an MCP call never marks the chat's `seen_` (`markSeen`), so the
  plugin's chat is sent a file Claude Desktop wrote; a chat turn never runs beside a call of Claude Desktop's (New
  chat and a session load keep `busy_` while one runs). Every other MCP result starts with a line when something
  changed since the last MCP call (`ChatAgent::mcpHint`, one cursor for the instance). The journal learns who: the
  editor's ports are `GesturePort`s (a dial moved between polls with one of them is the user's, else the host's),
  `setDraft` is the editor's and `chatSetDraft`, `chatRun`, `chatSetDial` the plugin chat's (or Claude Desktop's);
  `poll()` compares the dials, the bypass and the transport once a poll and takes as seen only the dials it set itself
  (`ownDials_`, `syncOwnDials`: a run's, a version's or a preset's, Claude's), so a move made meanwhile is journalled.
  The tools' dial lines say each value as the panel does (`dialText`, Controls.h: its unit, its option's name, the
  number after a scaled one). `plugin.live` checks both tools, `dsp.meters` LISTEN the fields, `plugin.chat` SEEN the
  two records.
- **MCP results** stay small: a picture is at most `kMcpPictureBytes` (75,000 bytes, 100,000 base64 characters:
  the PNG when it fits, else scaled down, else a JPEG, `chatimages::fit`), the text after it saying what was sent;
  view_panel and view_sound take MCP's own `detail: "high"` for the whole picture. The plugin's own chat keeps its
  pictures as they were. The guide comes in parts (`mcp/McpGuide.h`, cut at ChatPrompt's headings at run time, in
  the plugin and from guide.md in the server): "start" (working from outside, the contract, FMINUS, DSP PRACTICE,
  CHECKING YOUR WORK, the tools' notes, the other parts named), large-program (with THREADS), interface, library (a
  line a file), library:<file> (its header), library:<file>:code (its whole text, adding nothing to the editor),
  examples, projects, all. A heading renamed in ChatPrompt.cpp is a part lost: `plugin.mcp` GUIDE fails.
- **tune_dials** (`dsp/Tune.h`): Nelder-Mead over the named float dials' normalised ranges, from the dials as they
  are and a Halton spread, restarted when it settles with a goal unmet, until every goal is met, the budget (150, at
  most 400 renders) is spent, its time limit passes (`seconds`: 120, at most 170, under the MCP server's deadline)
  or the stop button cancels; deterministic but for the time limit. The best values go on the dials through
  `chatSetDial`, unless Claude Desktop cancelled the call (`mcpCancel`): then the dials stay exactly as they were.
- **The system prompt** (`ChatPrompt.cpp`) teaches Claude FMinus, the contract, how to build a large program, the
  `.gui` file's language (THE INTERFACE: GuiSpec.h; the `#pragma panel` lines only as the older way), the library
  (every file's header, from Library.h) and the examples, and shows a program built from the library for the
  product's kind (`ChatAgent::promptExample`: the program, its `panel.gui` and the library files it includes), which
  `plugin.chatexample` compiles and plays: a change to the contract (Patch.h), the panel's lines, the language's
  rules, a library file's header or the examples changes it too, and the example must keep compiling with no note.
- **Pictures** (`attach`/`detach`, `ChatImages.h`): read on the agent's job thread (`ChatAgent::queueJob`), kept as
  sent (the file's own PNG or JPEG bytes when they fit, else encoded again) with a thumbnail (RGBA8, and encoded for
  the session); sent as base64 image blocks before the message's text; six a message, twenty in the history. The
  editor draws thumbnails through FunkGui's image registry (`funkgui::images`, v0.20.0): `Panel::syncPictures`
  registers each picture's id while `chat_` holds it, counted across Panels (Picture.cpp makes one), so the last to
  let go removes it.
- **Web search** (`setWeb`, chat.json "web"): the API's server tools web_search and web_fetch (20260209) go in the
  request on a turn whose model takes them (`turnWeb_`), and a turn whose tools differ from the history's drops its
  thinking (`historyWeb_`, in the session as "web"). Their blocks (`server_tool_use`, the `*_tool_result`s with their
  encrypted parts, text with `citations`) go back as they came (`MessageBuilder` keeps them); `pause_turn` sends the
  history again with nothing after it. The sources listed under an answer are the citations' pages (the docs ask for
  them).
- **ask_user** pauses the turn: the reply's other tools run and their results wait in the history; the question's
  entry stays open (`ChatState::asking`) until `ChatPort::answer` (the labels clicked) or `send` (the user's own words,
  after a result that says so), and either starts the next turn with the question's result first in the user
  message (`missingResults`, `deliver`: tool results before any text, as the API asks). One ask a reply; the session
  keeps an open question ("asking"). The view draws it as a card (`ChatView`: ShapeKind::option, ActionKind::option and
  submit).
- **add_library_file** puts a library file in the editor as a tab (never over a file of that name) and returns its
  whole text, so Claude has seen it (`seen_`); the program's `#include` is Claude's to write.

## Recipes

- **An example**: a file `Source/fminus/examples/<key>/<n>-<name>.fm` whose first line is `// <Title>: what it is`;
  it appears in that product's EXAMPLES menu in file-name order (the first is the default program). Run its product's
  `fminus.examples` (`scripts/test.sh <key> -R fminus.examples`). Write it in the house style: the editor's Format
  (Shift-Alt-F) changes nothing in it, which `fminus.lang` checks for every example and library file.
- **A library file**: `Source/fminus/library/<n>-<category>/<name>.fm`, its first line `// <name>.fm: what it is`
  (the LIBRARY menu shows it, in both products), then `//` and a usage header (the comment lines it starts with, which
  Claude's instructions quote, and the editor's hover cards: a member's line, `filter.lp`, `y = filter.tick(x);`,
  then what it does after a gap of three spaces or more); in the house style (`fminus.lang`); names that no other library file uses; no allocation outside methods init and end
  call; safe before its set or allocate. Measure it in `fminus.library` (a harness there runs its modules over
  buffers), and run `plugin.chatexample` (the headers are in the prompt), for both products.
- **A new call into programs** (a note's own pressure for the synth, or one when the host's transport starts, say;
  what the host or the player is doing belongs in the Host block instead): add a `Call` row in `Patch.cpp` (the
  method, whether it is required, how the plugin declares it, the appended line), a pointer in `Patch::Compiled`
  looked up in `compile()`, a method on `Patch` the Engine calls; document it in Patch.h and the README.
- **A view**: add an `Item` in `Source/editor/Panel.cpp` (a FunkGui widget, or your own drawing on the
  `funkgui::Canvas`, under a tag of its own after `ui::kMarkTag` when a probe needs to find it) and place it in the
  constructor; then check `gui.dump`'s PNGs, both products'.
- **A .gui element** (a control, a display, a container): its `GuiKind` in `PanelSpec.h` and its word, arguments,
  properties and notes in `GuiSpec.cpp`, its natural size in `GuiLayout.cpp`, its item and drawing in `GuiItems` (a
  display's in `DisplayItem`), placed by `Panel::layoutGuiItems`; teach it in the prompt's THE INTERFACE
  (`ChatPrompt.cpp`) and GuiSpec.h's header; run `fminus.gui`, `gui.panel` and `gui.styles` (look at their PNGs) and
  `plugin.chatexample`. Shaders (`fminus/Shaders.h`: built on the compiler's thread with the bundled shaderc;
  `GuiShaderItem`, registered by `Panel::registerShaders`) have `gui.shaders`; the build copies shaderc into each
  bundle (`CMakeLists.txt`) and CTest gives the probes its path (`<prefix>SHADERC`). A property every element of a kind
  could take goes through `class` and `defaults` without more: they hand on properties, not kinds.
- **A dial style or a display kind** of the `#pragma panel` lines: its enum in `PanelSpec.h` (`DialStyle`,
  `DisplayKind`) and its word in `PanelSpec.cpp` (with a note where it does not suit), its size in `DialLayout`, its
  drawing in `Looks`, a display's motion in `DisplayItem` (a dial's in Panel.cpp's `DialItem`); run `fminus.panel`,
  `gui.dump` (look at `gui.dump.displays*.png`) and `plugin.chatexample`.
- **A RANDOM part**: a choice in `Random.cpp` whose lines name only what it declares itself, its name in `parts` and
  `allParts()`; `fminus.random` must reach it (raise `kSeeds` if no seed does) and play it cleanly. A library file
  RANDOM uses keeps its header's API, or Random.cpp changes with it.
- **A probe**: a file `Tools/probes/<layer>/<name>.cpp` whose first line is `// PROBE name=<layer>.<name> timeout=<s>`
  and whose body is `PROBE(<layer>, <name>) { ...; return P.finish(); }`; it runs for every product
  (`<key>:<layer>.<name>`), so it branches on `product::kIsInstrument` where the kinds differ and keeps any folder of
  its own apart from the other product's run (the product's prefix in its name). Use spec rows (`P.near`, `P.le`,
  `P.ge`, `P.in`, `P.eq`); helpers are in `Tools/probes/common/Audio.h` and `Tools/probes/common/Rig.h`.
- **A product**: `scripts/plugin.py add --from <key> --key <new> --name <Name> --code <Code>` writes
  `products/<new>.cmake` and copies the source product's examples and icon (same kind); CMake builds it with the
  others. Describe it in README.md and here.

## Git

Work on a branch; commit messages say what changed and why. Never commit build directories (`build-*/` is ignored).
Text about this repository's own history goes in `<!-- origin -->` blocks (README.md, CLAUDE.md), apart from what the
docs say about the code, so that a project made from this one can leave it out.
