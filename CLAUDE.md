# The playgrounds: rules for a Claude session

Two playgrounds for writing DSP while the plugin runs, built from one source tree: the effect, TestingPlayground
(`products/effect.cmake`), and the synth, TestingPlaygroundSynth (`products/synth.cmake`). Each is a plugin (AU, VST3,
Standalone; macOS arm64; and VST3, Standalone on Windows x64 and Linux x86-64: "Windows" and "Linux" below) on JUCE 8.0.4 whose FunkGui editor is a text field holding an FMinus program (the effect's
main module is `Effect`, one instance; the synth's `Voice`, one instance a note), which the plugin compiles (RUN,
Cmd-Return) and plays, its parameters becoming the dials. Made from FunkPluginTemplate. Change the panel, the examples
and the plumbing freely; keep the rules below. README.md is the user's guide (a short start: what the plugins do,
building from a clone, using them; then the detail: writing a program, the editor, the probes), CHANGELOG.md what each
day's merged pull requests changed for users (newest first, an Unreleased section on top); this file is what a session
must know (`scripts/plugin.py info` prints the identities).

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
  examples (`Source/fminus/examples/<key>/`) and its Claude Desktop extension's icon (`Source/mcp/icon-<key>.png`,
  which `Tools/logo/logo.swift` writes from its kind's mark; its app icon is its kind's, `Assets/logo/<kind>/`);
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
  (`Funk-<name>-mcp`) and the plugin's state format (its `<CHAT>` names a chat since the chats were kept on this Mac:
  an older one's conversation is adopted, below); and the chat's key and chats folder both share
  (`~/Library/Application Support/Funk/Playground/`; Linux's folders are XDG's: "Linux" below). CMake refuses two products with one name, code, bundle id or
  prefix.
- **The products' kinds differ where the README says**: the effect's `Effect` processes the input in place, has an
  `in` to display and passes the input through its bypass; the synth's `Voice` plays notes (MIDI, the panel's
  keyboard, the wheels, the sustain pedal), sixteen at once, and is silent in its bypass. Everything else, the editor,
  the chat, the projects, the MCP server, the library, is the same in both.

## What goes where

| Path | What | Rule |
| --- | --- | --- |
| `products/<key>.cmake` | a product's identity: name, kind (`effect.cmake` an effect, `synth.cmake` an instrument), version, company, codes, bundle id, environment and Objective-C prefixes | `scripts/plugin.py` reads and rewrites them; the plugin code stays unique among the user's plugins; nothing else names a product |
| `cmake/Deps.cmake` | the pins: JUCE, bgfx, FunkGui (v0.26.1: the TextEditor and its code-editor hooks, resizable Panels, custom shaders, images, several editors in one host, GPU pictures, Windows on Direct3D 11, unchanged frames left undrawn and each editor its own cadence, the TextEditor's kept spans, shader checks off the message thread, custom-shader textures, menus in the canvas's style, each frame shown as soon as it is drawn, primitives drawn as instances, custom shaders under Direct3D 11), FMinus (a commit) | move a pin's tag, commit and version together |
| `cmake/Probes.cmake` | each product's probe executable and tests (`fp_add_probes`), the lints once (`fp_add_lints`) | test names `<key>:<layer>.<name>`, labels `probe`, `<layer>`, `<key>` |
| `cmake/Installer.cmake` | the installer (`fp_add_installer`, after every product): its name (the products' names' common start, else the first's; `FP_PROJECT_NAME`, `FP_INSTALLER_NAME`, in the cache for `scripts/package.sh`), the generated `Installer.h`, the payload's `payload.json`, the app (target `Installer`), its payload (`installer_payload`: `cmake/InstallerPayload.cmake` clones every product's bundles and extension into the app, `ditto --clone`, then signs the app again, ad hoc) and its probes (`Installer_probes`: `Tools/probes/installer/`, tests `installer.<name>`, once); on Windows "<Project> Installer.exe" with `payload\` beside it (`InstallerPayload.cmake` copies each product's VST3 bundle, its Standalone's folder and its .mcpb, the build's .pdb, .ilk, .exp and .lib left out), nothing signed, the app and its probes on UTF-8 as their code page (`Source/installer/Utf8.manifest`) | built by every build (the clones cost nothing on APFS); the installer takes the first product's company, version, bundle id (`.installer` after it) and kind's icon |
| `factory/<key>/<name>/` | a product's factory projects (`plugin/Factory.h`): a folder a project, named as it is, holding `project.json` (a project file's JSON of one version, its files' texts left out) and that version's files beside it; CMakeLists.txt compiles them into the plugin (`generated/<key>/FactoryProjects.h`, its bytes printed at configure); `factory/<key>/imported.txt` lists what `scripts/import-factory.py` wrote | written by the import (a project folder by hand keeps the same shape); never the user's own projects nor the name map (both stay out of the repository: the user, or the lead when the user says, runs the import); the ones now are made-up placeholders the import's first real run replaces; `plugin.factory` compiles and plays every one |
| `Source/fsdsp/core/**` | FSynth's DSP core, vendored (`SEED.tsv`) | do not edit casually: `scripts/sync-dsp-core.sh` refuses to overwrite edits |
| `Source/fminus/**` | `Patch` (a program compiled for the plugin: the contract, the appended calls, the instances and the module Shared's one (an instrument's Shared's hooks and its params named as dials: below, "Shared on its own"), a mono program's `#pragma mono` and Voice's legato (below, "Mono"), the circuits' pots as dials (a main-module field of a netlist's module, unless the program calls its set(): Lang's `circuitDials`, checked against what FMinus compiled; the dials after the params; a nonblocking shim a field, `Patch::setCircuits`; `Recipe::bindings`, `CompileStatus::circuitDials`), the probe table (each main-module field of a netlist's module: its probe fields' offsets, checked, and the netlist's nodes; the audio thread's noexcept arm, disarm, count, ring and stats; `Recipe::probes`, `CompileStatus::probes`), the taps: the fields its displays read (the main module's, or Shared's through its pointer, `shared->x`: PanelSpec's `displayFields`), and a racing wheel's shift lights' (`lightsTap`: the field `shiftLights`, the main module's else Shared's, a float, int or bool; one of the name of another type a note), and what reading the panel again needs (`Recipe::panel`, `PanelBase`: the fields and where each sits (`FieldPlace`: an array's first element, its stride and count), the files FMinus read; `readPanel`, the one reader of a .gui file's panel, compile()'s and a panel's edit's), the field map (`fieldMap`, trace's: every scalar field as a path, a double read as one, an array of 16 elements at most whole and a 2-D one's by both indexes, each with its attributes' `FieldLook`, never a pointer followed, never a probe field), the Host block: host.fm, built in, `Patch::setHost`; the IR as text when asked: `compile`'s `keepIr`, `freeze`'s `ir`, and `Patch::ir` and `frozenIr`, the IR alone, compiled with the options the code came from); `PanelSpec` (its `#pragma panel` lines, read from the files it compiled: sections, dial styles and options, colours, displays, a number's `unit=`, `decimals=` and `scale=`, key=value with no space; a display's source a float, double, int or bool field or an array's element, `displaySource`, which GuiSpec and Patch's taps take too; a field's attributes, `FieldLook`: `ui::unit`, `ui::label`, `ui::decimals` and `fminus::range`, from FMinus's field list in Patch and from Lang's `Symbol::attributes` in the sandbox, giving a display what its line leaves out, a line's `unit=""` none over its field's in both readers: `unitGiven`; every text too long for its place (a title, a label, an option's name, a unit) cut where a character starts, `cutText`); `GuiSpec` (a `.gui` file, the first of its files: the interface's tree, in place of the pragmas, and the tapers, units and scales it gives the dials; `fminus.gui`); `HostFile.cpp` (host.fm's text, `Patch::hostSource`, apart from Patch.cpp so that `Lang` links without FMinus); `PatchCode.cpp` (`Patch::codeRanges`: where the program's machine code is, FMinus's code ranges named as the program names its functions, the appended calls as the methods they call, for the profile); `Circuits` (a `.cir` file, a SPICE netlist: the module FMinus's circuit compiler generates from it, generated when an `#include` first reaches it in a compile (`Generator`, one a compile: its errors stop the compile ahead of FMinus's, its warnings and the provisional note are notes, its jobs and keys go to `Patch::Result`), FMinus's messages on a generated module cited at the netlist's lines (`recite`), the self-test's answers, process-wide (`answers()`: 64, the in-flight set, `landed()`; each test's `Outcome`, its own warnings and why it gave no answer, `reported()`: a key given up on is not tested again, its compiles say `gaveUpNote` where the provisional note was; `erase`, an answer a compile found its netlist cannot take, which the Generator then generates without; `keysOf` and `restore`, what a session and a project version keep; `chosenNote`, a refine's note); the recipe keeps the texts (`Recipe::circuits`), so `freeze`, `ir` and `frozenIr` never generate; `fminus.circuit`); `LibraryIncludes.h` (`withLibraryIncludes`: a program's files and the library files it `#include`s, which EXAMPLES, the prompt's examples and the probes take); `ShaderLib` (what a panel's shader may `#include` besides the program's `.sh` files: `funk/text.sh` and `funk/shapes.sh`, written in bgfx's language as shaderc's HLSL front end reads it on every renderer (no vector from one number, no vector picked by `?:`, `texture2DLod`, loops of fixed counts, none of HLSL's own words such as `line`, `point` or `half`), and the font `text.sh` draws in: the atlas's grid, its numbers the bake's, the glyph codes a string becomes; `gui.shaders` TEXT); `Random` (RANDOM: a program for the product's kind from the library's building blocks; `fminus.random` compiles and plays 160 seeds); `Lang` (FMinus's language service, reusable by the chat's tools: its tokens, the program's symbols across its files, host.fm and the library, completion, signature help, hover, definitions, occurrences, an instrument's Shared hooks as declared (`sharedHooks`) and a mono program's legato (`legatoHook`); `LangConditionals.cpp`, the conditionals (`#if`, `#ifdef`, ... on the plugin's macros, `kDefines`: DEBUG, and the program's `#define`s): the groups a compile reads, which the index, the highlighter, the formatter and the panel's lines follow; `LangFormat.cpp`, the formatter, which checks that it changes no token; `circuitDials`, the dials a program's circuits get and why a field has none, which Patch and the GUI sandbox both take, and `potControl`/`switchControl`, their Controls; `fminus.lang`); `CirTemplates` (Play this circuit: a program.fm of the product's kind that plays a netlist, never calling its set(), in the house style; `fminus.circuit` TEMPLATE); `CirDoc` (a `.cir` netlist as the schematic sees it: its parts and nodes read with FMinus's token order (`read`), the drawing in its `*@` comment lines (`readLayout`, `writeBlock` and `withBlock`, canonical), `reconcile` (text to drawing, pure: lines bound by name, orphans adopted, session pins, the rest placed, every wired net repaired), the edits a gesture or Claude makes as text patches, all or none (`apply`, `parseOp`: edit_circuit's grammar), and the text helpers `stripLayout`, `sameNetlist`, `lineMap`, `sameProgram`; `fminus.schematic`) and `CirLayout` (the part library's symbols and pins in token order, the eight orientations, the full layout, incremental placement and the A* router, integer-only and deterministic, its budgets counted pops and its grid bounded (`kRouteCells`: a drawing whose parts span more is wired in groups split at its widest gaps, each on a grid of its own, the nets between them name tags, `Budget::far`; an op that would make one is refused), every point it writes one that reads back (`kPartLimit`)); `examples/<key>/*.fm`, a product's EXAMPLES (the first, `0-init.fm`, Init, the default program: the classic init patch; first line `// <Title>: ...`); `library/<n>-<category>/*.fm` and `*.cir`, the LIBRARY, every product's (first line `// <file name>: what it is`, a netlist's `* <file name>: what it is`, then its usage header; `fminus.library` measures each; a netlist pins its configuration, the self-test's, with `fminus <file> --self-test`'s line in its header) | JUCE-free; FMinus only through `fminus/FMinus.h` and its circuit compiler's `fminus/Circuit.h` (C++17, no LLVM; the latter in `Circuits.cpp` and `dsp/SelfTester.cpp` alone), never its internals (`circuit/` included) or LLVM's headers (`lint.layers`); `Lang` stays FMinus-free (the sandbox links it): a `.cir` is read line by line there, `isCircuitFile` is `dsp/Sources.h`'s; `CirDoc` and `CirLayout` are FMinus-free too (every layer may use them), outside `FP_SANDBOX_SHARED`, so `Lang` never includes them |
| `Source/dsp/**` | `Engine` (programs swapped by crossfade, the dials as glided streams, voices for an instrument (mono too) and its Shared's tick and note hooks, the output guard, the meters the displays read: `Meters` (`Meters.h`, apart so that what only shows them needs nothing of the audio side), their levels (`Levels.h`: a block's peak and energy in lanes) and the scope's rings; the schematic's probes: `setProbes`, the probes' rings, `Meters::probes`), `Compiler` (compiles off the audio thread, offers to the Engine, collects; the IR view's texts while someone looks: `setIrWanted`, `ir()`; the refine: the program that plays compiled again once a circuit's self-test answer lands, below; a panel's edit: a RUN of the panel's files alone applied to the program that plays with no compile, `editPanel`, below), `SelfTester` (the circuits' self-test, one a Compiler, on threads of its own: the jobs of the compile that plays, a test a key, answers into `circuits::answers()`; `fminus.circuit` SELFTEST, REFRESH and DEDUP), `Controls.h` (slot mapping), `Events.h` (the host's `Transport`, the program's `HostBlock`, an instrument's MIDI as events), `Measure` (a program rendered offline in an Engine of its own and measured in words and in named numbers: the chat's measure_output; `Session`, one compile measured many times through `Engine::restart`), `Goals` (goals and checks on those numbers: set_checks), `Tune` (tune_dials' search, and its sweep), `SoundView` (view_sound's render and analysis: `renderSound`, then `analyse`, which a recording takes too), `Compare` (compare's null test, gain and lag, bands, metrics; at two rates the levels, bands and metrics alone), `Debug` (trace: two renders through `RenderHooks`, the fields read through `Patch::fieldMap`), `Engine::Snapshot` (run_function's "playing": the state that plays copied by the audio thread at a block's end into the asker's buffers, below), `Export` (a render as data: the render hash, SHA-256 of its float32 samples L R interleaved, `Sha256`, `renderHash`, `hashLine`; a stereo WAV's writer, float32 or 24-bit PCM, and reader; a field log's CSV; through a `Sink`, so the plugin picks the file), `Profile` (measure_output's `profile`, opt-in: the thread that renders sampled by a thread of its own over the calls cpu_percent times, Mach's `thread_suspend` and `thread_get_state` on macOS, `SuspendThread` and `GetThreadContext` on Windows, each sample weighed by the thread's CPU time and its address mapped through `Patch::codeRanges`; below, "The chat"), `TextDiff` (a line diff: get_status's `since`), `Assets` (the samples: a file as added, its sound decoded, `Asset::at` its sound at a rate, resampled by a windowed sinc once a rate and kept; the caps and `refusal`'s words; a set immutable once made, `Assets` a shared pointer to one; `plugin.samples`), `Regex` (read_file's patterns: RE2's syntax, the part a search of code needs, compiled to a Thompson NFA and run as Pike's VM without captures, so a search costs at most the text's length times the program's size, whatever the pattern: never std::regex, whose backtracking a pattern such as `(a+)+$` makes exponential; each search counts its steps against the caller's budget; backreferences and lookaround refused) | JUCE-free C++20 |
| `Source/plugin/**` | the JUCE `AudioProcessor`: P1 to P32 (`ControlParameter`: the program's names and texts), one FunkGui port each, then the panel's own dials (slots 32 to 63: no host parameter, `panelValues_`, a `PanelDialPort` each; below, "The panel's own dials"), the program's files and the draft in the session, the bypass, the compiler's outcome polled on the message thread; `EditorPort.h`; the chat with Claude: `ChatAgent` (the conversation, the turn on its own thread, Claude's tools through `ChatHost`), `ChatPrompt.cpp` (the system prompt), `ClaudeApi` (the Messages API over HTTPS, to Anthropic), `Providers` (the chat's other model providers: an OpenAI-compatible server by its base URL, Google Gemini; a Messages API request translated into theirs and their streamed answer back into the Messages API's events, their models lists, their base URLs, keys and errors), `ChatProviderTurn.cpp` (ChatAgent's side of them: the provider chosen, its key, its models, a turn's requests), `ChatStore` (the key and the chats, encrypted on this Mac: below, "The chats: on this Mac only"), `ChatChats.cpp` (which chat shows, whose it is, when it is written, the chat list), `Factory` (the factory set as an immutable snapshot, `current()`, with a `generation()` that moves with it: the set fetched from the website once the process's store has one, else the one compiled in, read once; `ProjectStore` serves them by their factory name, read-only), `FactoryStore` (the factory projects from the website: fetched, checked, kept in the user's data folder, read back and published to `Factory`: below, "The factory store"), `ChatImages` (pictures for a message: read with JUCE, Core Image for WebP and HEIC, scaled, thumbnailed; an MCP result's made to fit), `SoundPicture` (view_sound's PNG, drawn with JUCE's software renderer on the agent's worker, and its thumbnail; compare's), `ChatScenario` (the render tools' `scenario` and `try_edits`, measure_output's `spectrum` and trace's `fields`, `voice` and `export`, read from JSON with juce_core and the DSP library alone: the headless renderer compiles it, `FP_RENDER_SHARED`), `ChatCompare` (compare), `ChatTrace` (trace), `ChatFunction` (run_function), `ChatImport` (write_file: one file, several as one change, a file or folder of the user's brought in, the editor's files out to a folder), `ChatCaptured` (the recording as a render: the capture's seam; `scenario::resolve` and `evaluate`, what of a render's settings reaches the plugin), `ChatProjects` (the projects tool) and `ProcessorSaves.cpp` (its processor side, on the message thread: below, "The chat"), `CaptureReplay` (a take replayed, its null test, its analysis), `ChatPort.h`; `McpLink` (the instance's socket for the MCP server: info, tools, guide, call and cancel; a call through `ChatAgent::mcpCall` on its worker, the quick ones on its poll thread; its name (the user's, kept with the session, else its number's), its card and the others' read about once a second, the user's choice of Claude Desktop's target, what Claude Desktop's calls do there: `EditorPort::desktop`); `StandaloneMenu` (the Standalone's menu bar as data: `gui.window`); `DevicePort.h` (the Standalone's audio and MIDI devices as the Settings card sees them, plain data and actions, JUCE-free: EditorPort.h includes it); `WheelMap` (a Logitech G29 racing wheel made MIDI, JUCE-free and IOKit-free: its modes told apart and hid-lg4ff's commands (the shift lights' report, `lightsReport`, written from memory and checked on no wheel: its last byte, `kLightsLast`, the doubt; `lightsMask`, a program's `shiftLights` as the mask), the one table of its HID elements (`kAxisRows`, `kGears`, `kBits`, the start key), `Map`, its values to channel 1's control changes and the start key's note, sent on change (a pedal's first 1 % released, a change under `kJitter` held back, `resend`); `plugin.wheel`); `FolderSync` (the program's files mirrored to a folder on disk and synced both ways, JUCE-free: below, "The code editor"; a pass on the processor's timer, `Processor::folderDraftIf` its version-guarded write of the draft, `<FOLDER>` in the session; `plugin.folder`), `FolderWatch` (FSEvents' word that a folder changed, macOS; none elsewhere: FolderSync looks every half second); `Launch` (a program started from an argument list, never through a shell, JUCE-free and POSIX: Linux's Show in File Manager and Open in VS Code, "Linux" below); `ProcessorAb.cpp` (A/B listening: below, "A/B listening"; `plugin.ab`) and `AbMatch` (its loudness: each side rendered offline in a `measure::Session` of its own, BS.1770's integrated loudness of a chord held or of pink noise through the effect, the louder turned down; on a thread of its own, the newest job only, JUCE-free); `WheelDriver` (the Standalone's wheel as decisions over a `wheel::Transport`, JUCE-free and IOKit-free too: the seed rule (no value IOKit holds before a report, its timestamp 0), the compatibility mode switched at most once each time the entry is switched on, the pending switch kept and listed while the wheel re-enumerates, the range before the spring, the spring released on every way out that has a device and its failure on the card, the rate (`kGather`, `kInterval`), `resend`, the shift lights (`Transport::lights`, looked at every `kLightsInterval` while read, sent only when they change, none sent for a program that never lights them, off after the spring on every way out that has a device), the card's line; `plugin.wheel` DRIVER drives it with fake devices; its commands through the transport's `setRange`, `setSpring`, `setLights` and `toNative`, IOKit's sending hid-lg4ff's reports, and its errors in the transport's words, `describe`); `WheelEvdev` (Linux: the driver's transport over the kernel's hid-lg4ff, JUCE-free: the wheel found in sysfs, its evdev node read (never grabbed) and its codes made HID usages again, the range through sysfs's `range` (read first, written only when it differs), the spring as `FF_AUTOCENTER`, the lights through the `::RPM<n>` LED files, the switch through `alternate_modes`, SYN_DROPPED read again; the event loop a `System` of its caller's; `plugin.linux` WHEEL drives it over a made-up sysfs tree and socket nodes); `SecretStore` (a keyring for the chat's keys: Linux's Secret Service through libsecret, dlopen'ed, its C API declared in the file (no header or link), the session bus's address or `$XDG_RUNTIME_DIR/bus` required first so GLib never autolaunches a bus, `has()` never unlocking; `ChatStore` keys go there when it is available and never from a test's folder, a file otherwise; `plugin.linux` SECRETS); `MidiLearn` (the dials' MIDI learn, JUCE-free: below, "MIDI learn"; `plugin.midi`); `AssetFiles` (a sample's file decoded by JUCE's WAV, AIFF and FLAC readers alone, read from disk with its size checked first, base64) and `ProcessorAssets.cpp` (the samples: below, "Samples"; `plugin.samples`); `Verify` (SHA-256 and ECDSA P-256, JUCE-free, each system's own library: below, "Factory projects"; `plugin.verify`) and `FactoryKeys.h` (the public keys a factory index is taken from) | reads parameters once per block, calls the Engine; copes with what a host may do (below); the chat's rules below |
| `Source/plugin/standalone/**` | the Standalone app, compiled into each product's Standalone target only (`JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP`): `App.cpp` (its `JUCEApplication`, window and content (the editor alone), with `juce::StandalonePluginHolder` for the devices and the state, in JUCE's own settings file and keys: `filterState`, `audioSetup`, `shouldMuteInput`, `windowX`/`windowY`; the menu bar's commands, Settings... asking the editor for its card: `Processor::showSettings`), `Devices` (the `DevicePort` over the holder's `AudioDeviceManager` and its input's mute, as JUCE's dialog had them: its channel limits, pairs and names, `setAudioDeviceSetup`'s error, the lists followed through the manager's and each type's listeners and `juce::MidiDeviceListConnection`; `audioSetup` and `shouldMuteInput` saved on every change, not only at shutdown; an instrument's racing wheel listed after the MIDI inputs, its choice under `racingWheel`, its messages posted into the holder's player's `MidiMessageCollector` as JUCE's MIDI inputs feed it, and sent whole again (`resendWheel`) after each of the device manager's change messages (a restart resets the collector, dropping what waited) and after Reset to Default State; `gui.settings`), `RacingWheel` (`RacingWheelLinux.cpp` on Linux: `plugin/WheelEvdev.h`'s hub on JUCE's message loop, its nodes and an inotify watch of `/dev/input` through `LinuxEventLoop`'s descriptor callbacks, the driver's timers JUCE timers; on macOS a Logitech G29 over IOKit's IOHIDManager, C API, on the main run loop in every mode: `plugin/WheelDriver.h`'s transport (the reports, the values with their timestamps, the input callback, two timers), the driver deciding (a compatibility-mode G29 switched to its own mode once each "on", its range and spring set and the spring stopped when it is off or the app quits, its values through a `wheel::Map` at most 125 times a second, put back at rest when it goes or is switched off; its shift lights the program's, `Devices`' hook reading the processor's Engine's meters, `Meters::lights`); a crash leaves the spring on until the wheel is unplugged; IOKit linked into the Standalone only), `NativeWindow.mm` (Objective-C++, ARC: the window's transparent title bar over full-size content, a view of ours in the title bar that keeps the buttons where the editor wants them, the drag and the double-click, the menu bar) | the AU and the VST3 never include it; the editor learns of the window only through `EditorPort::windowButtons`, `placeWindowButtons`, `dragWindow`, `devices` and `settingsAsked` (`Processor::setWindowHooks`, `showSettings`; none in a host: no card, no notice); nothing in the window is drawn by JUCE but the window itself, the native file choosers and the menus (`showAudioSettingsDialog` is called nowhere); the devices are the message thread's alone; a key the editor uses never becomes a menu shortcut (the editor gets every key first) |
| `Source/editor/**` | the FunkGui `Panel`, resizable (`layout()` places every item for the size; `kMinWidth` x `kMinHeight`): the top bar (the product's mark and name, the project's pill and, while one is open, its presets right of it (the arrows and the preset's name: "The presets in turn" below), the Code / Panel switch, the load (a snowflake left of it while the frozen copy plays, `EditorPort::frozen`; `gui.dump.frozen.png`), the theme; the status bar's REC and Mark while the capture tool records (`EditorPort::recording`, `markRecording`; Cmd-Shift-M; `gui.dump.recording.png`); in the Standalone, room at its left for the window's buttons, and its empty parts drag the window: `EditorPort::windowButtons`, `gui.window`), the explorer's card at the left (`Explorer`: the program's files, its samples (ASSETS: `EditorPort::assets`, a row each with its length, ticked when the program that plays reads it, + a chooser, the cross removing one, a file dropped on the panel added: `Panel::addSamples`; `gui.explorer` ASSETS), its folder (FOLDER: `EditorPort::folder`, the rows that turn it on and off, open it, tick Run on save and settle a conflict, its notes as captions; `gui.folder`), Claude Desktop (CLAUDE DESKTOP: `EditorPort::desktop`, read each refresh, the instance's name (a click opens `NameCard`, the name card), Use with Claude Desktop (ticked while it is the target), Claude Desktop's last call there (lit while it acts on it, its time in words that change at most once a minute; a click opens the chat), the others open (a click makes one the target); none without a socket; with the chat header's subtitle and badge, and the badge's mark on the folded chat's rail, `gui.desktop`), its LLVM IR (the IR view's rows), the project open's presets, the projects, the library and the examples in sections that fold, its rows doing what the tabs, PROJECTS, LIBRARY and EXAMPLES do through the Panel's hooks; it folds away, its button then at the left of the program's card's header, `EditorPort::explorerOpen`; it is away in the PANEL view and when the program's card would keep less than `kProgramMinW`; `gui.explorer`), the columns' widths (`Columns`: the explorer's and the chat's, a share of the window until the user drags the explorer's right edge or the chat's left one (`EdgeItem`: a band over the gap, a hairline lit on hover, the arrows, Home, End, Delete, a double-click resetting, a slider to a screen reader), each within its least and its most; the program's card keeps `kProgramMinW`, the explorer giving way first, then the chat, then the explorer folding; `fit` is the arithmetic and every view takes it, the PANEL view with the CODE view's chat (its width and its edge's reach, where the explorer may have taken some of its room: a switch of the view then never lays the conversation out again, `ChatView::blockLayouts`, gui.switch SWITCH; the editors keep the CODE view's place while the PANEL view hides them, so no width set measures their lines again); only a width that moves an edge is set (a step at the reach keeps the width asked), and a hidden edge gives up the focus; the widths set kept in the session, `EditorPort::explorerWidth` and `chatWidth` (0: none), and as this Mac's default for an editor whose session has none, UiPreferences' `explorerWidth` and `chatWidth` (a drag's on release, a key's or a screen reader's once the steps pause, `kDefaultPause`; read again in `attach`, after EditorHost's reload); a Panel takes the port's widths when another Panel or a session load changes them; the edges are hit before the cards' items but under the code editor's overlays (`FilesItem::overlayAt`); `gui.columns`), the program's card (its status and actions: Examples, Library, Random, Run; the files' tabs, a `funkgui::TextEditor` each made a code editor by `CodeAssist` (FMinus's highlighter, the problems' squiggles and marks from RUN and from the check while typing, completion, signature help, hover cards, Format Document and Format on Run, the find bar, the line commands, go to definition, go to a line (Ctrl-G), the status line; the completion list, the signature and the hover card drawn over everything; `gui.code`), the IR view (`IrView`: a read-only tab apart at the tab bar's right, a TextEditor a view, its text and its LLVM IR highlighter; `gui.ir`), a netlist's Schematic view (`SchematicView`: a .cir tab's drawing, switched with its Netlist view by the pills at the tab bar's right or Cmd-Shift-D, which FilesItem hands draw, input and keys; its gestures made `cir::Op`s and one `TextEditor::applyEdits` each; a knob beside each pot and a chip beside each switch the program that plays drives by dials (`CompileStatus::circuitDials`), writing through the Panel's GestureController on the dial's own port; its live probes (the selected net, up to three pinned lanes, a readout card over the canvas, resolved by name in `CompileStatus::probes`, the Panel's own set through `EditorPort::setProbes`); Play this circuit (`fminus/CirTemplates.h`: program.fm replaced as one undo step of its own, Undo in the info strip); the room it takes (the problems list hidden, the editor at least max(312, 55 %) of the column, the dials folded into a bar when they do not fit); its fields (a part's names and values edited in place, the Label tool's net name), its clipboard of netlist text (`cir::copied`), Show as labels and wires; `SchematicDraw`: its symbols, wires, flags, labels and marks, and `still`, a picture's whole drawing; each file's view, zoom and pan in the session, `EditorPort::fileViews`, written on the user's gestures only; its session pins, `EditorPort::setSchematicPins`; `gui.schematic`), the problems list, the dials (`DialLayout`: plain rows, or the panel's sections; `Looks`: the dial styles and the displays drawn; `DisplayItem`: a display's motion, and `Spectrum`, a source's spectrum: a real FFT over tables made once, run when its samples change and shared by the source's displays), or a `.gui` file's interface (`GuiView`: its items made, placed, fed and drawn, which the GUI sandbox uses too; laid out once at no scroll, a scroll only moving the items by whole px and placing them again, `Panel::scrollGui` laying nothing out: `Panel::layouts`, gui.panel SCROLL; `GuiLayout`: its flexbox layout; `GuiItems`: its controls in their looks, pad, envelope, keyboard (an instrument's), tabs and frames; `gui.panel`, `gui.styles`), its pages kept by title (`EditorPort::guiPages`), and the Code / Panel view (the session keeps it: `EditorPort::panelView`); the panel under the code (the status line's Panel switch, `CodeAssist::setPanelSwitch`, and Cmd-J: in the CODE view the dials, the displays, the interface and a netlist's fold bar hidden, the editor or the drawing taking the column; two choices, the code's and the interface files' (`isInterfaceFile`: .gui, .sc, .sh), the tab shown picking one, kept in the session, `EditorPort::codePanel`; `layoutStale` lays out again when the tab shown wants another room; the PANEL view and view_panel's panel pictures always show it; `gui.codepanel`)); the code window (the top bar's window button, left of the Code / Panel switch: the code editor and the explorer in a top-level window of their own, a Panel of `Role::code` (no chat, no panel under the code, its own least size), which holds the editor itself, never a copy (`lendEditor`, `takeEditorBack`: the FilesItem and what follows it change hands, its hooks bound again, `bindFiles`); the editor's window meanwhile laid out as the PANEL view (`full()`, the session's view untouched), its Run forwarded; the session's choice, size and place, `EditorPort::codeWindow`; `gui.codewindow`); the chat's card, which folds into a rail (`EditorPort::chatOpen`); the chat's card (`ChatView`: the conversation, laid out and drawn as a chat app does; `ChatItems`: the message box and the chat's buttons; `ChatStyle.h`: the rounded look, its palette mixed from the theme's tokens) and the key dialog (the model provider's settings too: its Provider pill, a dropdown; another provider's address, key and model; `gui.providers`); `PresetPicker`, the top bar's list of the project open's presets (under the name between the arrows; a search, the presets in groups by their category, a row's menu setting one; `gui.presets`); `ProjectBrowser`, the projects' card (the projects in groups that fold, a row's star, the projects chosen (Cmd-click, Shift-click) given a category by its context menu, a drag onto a group or typed in its field, the search, the tag filter and the order, as this Mac's view keeps them (`EditorPort::projectsView`); the chosen one's versions and presets, Import, Export and A/B...; its menus the host's, an answer calling the Panel's `afterProjects` through `setAnswered`; `gui.projects`) and `ProjectGroups` (the groups, one way for it and the explorer's PROJECTS, whose captions fold the same groups); `AbCard`, A/B (two versions, or the editor's files and a version, played in turn through `EditorPort::ab`: the versions with their A and B chips, what changed from A to B a tab a file (`textdiff::diff`, three lines of context), A and B, the dials, Match loudness, Keep A, Keep B and Close; it follows the processor's A/B each frame and closes itself when it ends on its own; `gui.ab`); `ExportCard`, EXPORT (a project's info, its two exports); `Dropdown.h`, every dropdown's trigger and the menus' rows (below, "The code editor"; `gui.menus`); `SettingsCard`, the Standalone's Settings over `EditorPort::devices` (Output, Input for the effect, Audio, MIDI for the synth; the host's menus; modal; it scrolls when the window is short; opened by the app's Settings... (`EditorPort::settingsAsked`, polled each refresh) and by the effect's muted notice at the status bar's right; `gui.settings`); `ShaderFont` (`funk/text.sh`'s font: FontService's atlas laid out again on `ShaderLib.h`'s grid, registered as FunkGui's texture once a process and bound to each shader `GuiView::registerShaders` registers, which the sandbox's helper shares); `Picture.cpp`, view_panel's picture (a headless Panel on the message thread, for `ChatHost::chatPicture`, drawn on the GPU: `Picture.h`, below; its view "schematic", `pictureSchematic`, a `SchematicPicture` of its own; `PictureDraw.cpp`, the frame drawn and its PNG, the sandbox's too); `Items.h` (and `Items.cpp`: `asciiText`); `Theme.h` (each product's accent: the effect's FunkGui's orange, the instrument's a blue with orange errors; `signal` is the errors' alone, and `codeSecond` the code's types and notes; the colours are a kind's, `kindTheme`, with no Product.h, so the installer draws each product's mark in its accent; `Theme.cpp` holds `productTheme` and `codeSecond`, the product's); `LogoMark.h` (the top bar's mark, generated by Tools/logo/logo.swift: an `areaStrip` for the wave and one primitive for the caret, drawn as they are under `kLogoTag`, the 1x drawing at a `Canvas::dpi()` of exactly 1 and the 2x one at any other: its hints hold at a zoom of 100% and 200%); `gpu/` the EditorHost factory (the window's size goes to the session) and `CodeWindow` (`ui::Editor`, the plugin's EditorHost, which opens the code window: a second EditorHost in a JUCE window that follows `EditorPort::codeWindow`, made and let go after the event that asked) | reaches the processor only through its ports and `EditorPort` (the chat through `EditorPort::chat()`) |
| `Source/mcp/**` | `<Name>MCP` (TestingPlaygroundMCP, TestingPlaygroundSynthMCP), the MCP server Claude Desktop starts (`Main.cpp`: JSON-RPC 2.0 on stdio, one `poll()` loop over stdin, the instances' sockets and the GUI sandbox's helper's pipes; list_instances, playground_guide and gui_sandbox, the server's own, and the chat's tools, routed to the instance named, else to Claude Desktop's target, else refused; the prompts), `McpProtocol.h` (the private folder, the sockets, the line protocol, shared with `plugin/McpLink`), `McpTarget.h` (each instance's card, `<n>.json`, and the user's choice of target, `target.json`, in that folder; the target rule; a name's refusal: both ends read them), `McpGuide.h` (the guide cut into parts, shared too) and `McpSchema.h` (MCP's copy of the tools' schemas: `dropRepeated` and the annotations, which the plugin's tools.json and the server's list both take); the build packs it with the product's `icon-<key>.png` (Tools/logo/logo.swift's), the sandbox's helper, the headless renderer (`<Name>Render`) and the licences of what the helper holds (`licences/`: bgfx's, bx's, bimg's, bgfx.cmake's and the font's, as `funkgui_add_font` gives each bundle; `scripts/package.sh` packs them again when it signs) as `<Name>.mcpb` (a Claude Desktop extension) | juce_core and POSIX only (and the generated `Library.h`): no plugin code; stdout is the protocol's, written by the loop alone |
| `Source/installer/**` | the installer, one app for every product ("<Project> Installer": `cmake/Installer.cmake` works the name out from the products' names, and nothing here names a product): `Engine` (no GUI, juce_core: the payload (payload.json and the bundles beside it, the build's), what is installed where in both scopes with its version, the plan (install, upgrade, downgrade, rebuild (the CDHash, else the bytes), same, blocked and why), installing (ditto into a staging folder beside the target, then one rename: `renamex_np`'s RENAME_SWAP over an old copy, which goes to the Trash), uninstalling (to the Trash), the "All users" script, the notes on running apps and hosts, the log; `Engine.cpp` what both platforms share, `EngineMac.cpp` and `EngineWin.cpp` each one's own (`EnginePlatform.h`: a path's kind and owner, renames, versions, places, hosts; "All users")), `System.cpp` (`MacSystem`: ditto, pgrep, killall, the Trash, osascript), `SystemWin.cpp` (`WinSystem`: robocopy, the process list, the Recycle Bin, UAC through PowerShell), `Panel` (its window: a FunkGui Panel in ChatStyle's look, Welcome, Choose, Installing, Done, Uninstall; runs on a thread of its own), `App.cpp` (the app: an `EditorHost` over a processor that is none, in `plugin/standalone/NativeWindow.mm`'s window; on Windows JUCE's window with Windows' title bar, the Panel in FunkGui's `EditorHost` (Direct3D 11), or in `editor/nogpu/SoftEditorHost` with `FP_WITH_GPU` off; the theme from the system's appearance, held in memory) | the rules under "The installer" below; it includes from `editor/` only its JUCE-free, product-free headers (`ChatStyle.h`, `LogoMark.h`, `Theme.h`'s kind colours) and, with `FP_WITH_GPU` off, the product-free `nogpu/SoftEditorHost` |
| `Source/sandbox/**` | `<Name>GuiSandbox`, the GUI sandbox's helper, which the server starts on demand (`Main.cpp`: JSON lines on its stdin and stdout, one request at a time, `SandboxProtocol.h`; its main thread JUCE's message thread); `Sandbox` (the document: the .gui and the program files beside it, read never compiled, GuiSpec's notes, the layout report (its fit the check run_program makes: GuiLayout.h's `guiFit`), the render), `Dials` (the dials from the program's declarations through `Lang`, its circuits' after them as the plugin makes them (`lang::circuitDials`), or Claude's list, mocked), `View` (a `funkgui::Panel` of the area's size holding the editor's `GuiView`, its dials mock ports, its displays demo meters) | the editor's .gui code and the .gui and program reading by name (CMakeLists.txt's `FP_SANDBOX_SHARED`) and FunkGui: no plugin, audio or compiler code, no FMinus (`fminus/Circuit.h` and `fminus/Circuits.h` neither) or LLVM (`lint.layers`) |
| `Source/render/**` | `<Name>Render` (TestingPlaygroundRender, TestingPlaygroundSynthRender), the headless renderer (README, "The headless renderer"; `Main.cpp`): a program's files (a folder or the files) and the tools' JSON in (read by `plugin/ChatScenario.cpp`'s readers, which it compiles: `renderOf`, `dialsFrom`, `spectrumOf`, `profileOf`, `watchOf`, `exportOf`, `tryEdits`, `fits`; no plugin, so "host" is the defaults, and "capture" and a program that reads samples refused), measure_output's report and metrics (`measure::Session::measure`) and trace's export (`measure::trace`, `exporting`: the WAV, the CSV) out, the same bits as the tools; exit 0 rendered, 1 a usage, input or output error, 2 not compiled, 3 stopped (a loop past its budget, a failed bounds check, an init's: `Session::initStopped`); in each product's extension beside the server (`server/<Name>Render`), signed with `scripts/entitlements/render.plist`; `plugin.render` runs it against the tools | juce_core and the DSP library: no processor, editor or FunkGui (`lint.layers`: of the plugin `plugin/ChatScenario.h` alone, and `ChatScenario.cpp` itself includes of the plugin only its header and `ClaudeApi.h`, whose `object` is inline); a reader the renderer needs goes into `ChatScenario.cpp`, shared with the tool that takes it, never a copy; what needs the processor goes into `ChatCaptured.cpp` |
| `Source/editor/nogpu/**` | the editor's host on the CPU, for a build with `FP_WITH_GPU` off (an option, on by default on every platform): `SoftEditorHost` (EditorHost's rules over FunkGui's SoftRaster in a JUCE component), its factory `CreateEditorSoft.cpp` (in place of `gpu/CreateEditorGpu.cpp`), `ShaderLibraryNone.cpp` (`funkgui/gpu/ShaderLibrary.h` with no renderer: the plugin, the probes and the sandbox's helper take it, `FP_NOGPU_SHARED`) | below, "Windows" |
| `Tools/wheel-dump/` | wheel-dump, a developer's tool: a connected G29's HID elements and live values beside `WheelMap.h`'s table, the MIDI it would make, `--leds` its shift lights tried (README, "A racing wheel") | a CMake target outside `all` (`--target wheel-dump`), never installed nor run by a test; it shares `WheelMap.cpp`, so a table row it shows wrong is fixed there |
| `Tools/logo/logo.swift` | the mark ("Caret": a wave written up to the editor's caret), its one source: the construction in its header (the 3-4-5 arcs, the effect's processed trough, the optical corrections, the hinting of every size, the colours) and every number, and what writes every asset from them: `Assets/logo/`, `Source/mcp/icon-<key>.png` and `Source/editor/LogoMark.h`; it reads `products/*.cmake` and the instrument's accent in `Theme.h` | `swift Tools/logo/logo.swift` after any change (it needs JetBrains Mono: a build's FunkGui checkout, or `--font <ttf>`; `--out <dir>` writes elsewhere, `--preview <dir>` adds enlargements); the same bytes run after run; look at `Assets/logo/sheet.png` and `construction.png`; never edit its outputs by hand |
| `Assets/logo/**` | its outputs: `<kind>/` (`effect`, `instrument`: a product's `PRODUCT_KIND`) the app icon (`icon.iconset`, 16 to 1024 px: every size from 64 up on the pixel grid (the crest, the trough, the floor and the stem on whole px), 32 hinted on its own grid, 16 set pixel by pixel; `icon.icns`; `icon.svg`; `installer-1024.png`, the master an installer takes), `extension-512.png`, the mark's SVGs (`mark-graphite`, `mark-paper`, `mark-flat`) and lockups; `readme-*.png` (the README's header: both marks), `construction.png`, `sheet.png` (the top bar's tiles in FunkGui's own pixels: logo.swift mirrors its rasteriser) | generated, committed. CMakeLists.txt gives JUCE `installer-1024.png` as ICON_BIG and puts `icon.icns` over the Icon.icns JUCE makes, in every bundle (the Standalone's `CFBundleIconFile`; the About box shows it) |
| `Tools/probes/**` | probes on FunkGui's Harness, each run for every product: `fminus/` (the examples, the contract, the library, the panel's lines), `dsp/` (the Engine, its meters, the Host block), `plugin/`, `gui/`; `common/Rig.h` runs a program in an Engine, `common/Cadence.h` an editor's frames at FramePump's cadence, `common/Probe.h`'s `probe::skip` says a row a machine cannot judge (and `emulated`, `noDisplay`, `common/Pictures.h`'s `noGpuPictures` say why: "Linux"); `installer/`, the installer's (no product's: their own executable, each run once; `Fixture.h`, their payload, root and `System` stand-in) | spec rows only, no golden files; where the kinds differ, a probe branches on `product::kIsInstrument` |
| `Tools/probes/fixtures/factory/` | the factory set's test fixtures, made by the website repository's `scripts/factory-fixtures.ts` (its README, "The plugins' test fixtures") from `factory/`: known answers for SHA-256 and ECDSA P-256 (`vectors.json`), the test keys' public halves (`keys.json`), and a little signed site per case with what a plugin must do with it (`cases.json`; the folder's README says each); `plugin.verify` and `plugin.factorystore` read them through `PROBE_SOURCE_DIR` | made there, never by hand: regenerated as a whole (fresh keys, so every signature changes; the cases stay) when a check in the website's `scripts/factory.ts` changes; no private key, nothing of a user's |

There are no starters here (`scripts/starters/` is the template's, and new plugins start from FunkPluginTemplate).
`plugin.py add` makes another product of this tree from one of these (its kind, a copy of its examples and its icon;
its plugin code must be free across `~/audio/plugins` and here); `plugin.py rename` gives a product a new identity in
place (its `products/<key>.cmake`, and its names in the docs).

## Build and test

- Windows: `cmake --workflow --preset windows-verify` (or `windows-effect-verify`, `windows-synth-verify`; the release
  build `windows-release-build`), in `build-windows-<preset>`, from an x64 Native Tools prompt ("Windows" below).
- Linux: `cmake --workflow --preset linux-verify` (or `linux-effect-verify`, `linux-synth-verify`; the release build
  `linux-release-build`), in `build-linux-<preset>`, with clang and lld, on an X display that runs a window manager
  (with `$DISPLAY` unset the menus' pictures are skipped, saying so; a bare X server ends the process: "Linux" below).
- `cmake --workflow --preset dev-verify`: configure, build (both products' Standalone, AU, VST3, MCP server, headless
  renderer and probes, and the installer) and run every test in `build-dev`: each of the 71 probes once for each product
  (`effect:fminus.examples`, `synth:fminus.examples`, ...), and the lints and the installer's probes once
  (`installer.engine`, `installer.gui`: `build-dev/probe-out/installer/gui.installer*.png`, look at them after any
  change to its window). Done means it exits 0 with no warning from our sources (they build with `-Werror`). While
  working on one product, `cmake --workflow --preset effect-verify` (or `synth-verify`) builds and tests it alone (the
  build preset and target `<key>`, the test preset `dev-<key>`: label `<key>`); the gate is still dev-verify.
- `scripts/test.sh [effect|synth]`: build and run the probes only, of both products or the one named (`-R <name>`,
  `-L <label>`, `-V` pass through to ctest). Run the probes through ctest only: outside CTest a probe's stores are the
  user's own folders. The editor rendered headless lands in `build-dev/probe-out/<key>/` (`gui.dump*.png`,
  `gui.code*.png`: the code editor's completion, signature, hover (and a long declaration's on two lines, a local
  named as a constant drawn plain: `.hover_long`; that local's card: `.hover_local`), problems, find bar and PAPER;
  `gui.ir*.png`: the IR view's optimised, lowered (the find bar) and frozen views, PAPER, and its tab beside sixteen
  narrowed ones;
  `gui.schematic*.png`: a netlist's Schematic view, the library's netlists drawn, PAPER, the least window with 32 dials
  folded and with sixteen tabs and the IR tab, a wire and a move mid-drag, the problems' marks, an empty netlist, 25 %
  and 400 %, the circuit dials' knobs, a netlist after Play this circuit, a part's field with the `1M` warning, a net
  named and shown as labels, a playing netlist's probes (a readout card and two pinned lanes), a netlist no program
  #includes with a note (the note at the info strip's left, Not played at its right);
  `gui.chat*.png`: the empty chat, a conversation, the key dialog with a key pasted and with one stored, PAPER, the
  least size and a large one; `gui.providers*.png`: the key dialog as the provider's settings (Anthropic's with its
  Provider pill, OpenRouter's rows and PAPER, Ollama's, Gemini's, another server's refusal), the chat's bar, its
  Thinking pill on an OpenRouter model that reasons (`.effort`) and a turn on OpenRouter; `gui.columns*.png`: the explorer's and the chat's widths dragged, an edge lit (and in PAPER) and
  one focused in PAPER, a wide, a narrowed and the least window (the give-way order), the PANEL view and a Schematic view;
  `gui.codepanel*.png`: the panel under the code hidden (and in PAPER), the status line's Panel switch under the
  pointer, the program's card at its least (the switch's icon alone), the least and a wide window (and in PAPER), a
  `.gui` program's code with its interface hidden and its `panel.gui` with it shown, a Schematic view;
  `gui.codewindow*.png`: the window button under the pointer, the editor's window while the code is in its own (and in
  PAPER), the code window (and in PAPER) at its default, least and a wide size, a `.gui` program's interface full size,
  the code window with the IR tab;
  `gui.folder*.png`: the explorer's FOLDER section off, on with a save landed, a conflict and a note (and in PAPER), a
  wide and the least window;
  `gui.desktop*.png`: the explorer's CLAUDE DESKTOP (named, the target, a call acting on it, another instance; and in
  PAPER), the chat header's badge while another is the target, its mark on the folded chat's rail, the name card;
  `gui.ab*.png`: PROJECTS with A/B..., A/B's card with B playing and matched (and in PAPER), a file only A has
  scrolled, the least and a wide window;
  `gui.settings*.png`: the Standalone's Settings card, PAPER, the least size, an error, the effect's muted notice;
  `gui.menus*.png`: each dropdown's menu over its pressed trigger (and PROJECTS' context and tag menus, the thinking
  levels of an OpenRouter model: `thinking_openrouter`), both themes;
  `gui.projects*.png`: PROJECTS' groups (favourites, the factory's, categories, uncategorised; one folded, two chosen),
  a category typed, two projects dragged over a group, a search with a tag, the explorer's PROJECTS grouped, each in
  both themes;
  `gui.presets*.png`: the top bar's presets (the arrows and "Bright 2/4"), the modified dot, "(no preset)", their list
  open (its groups, the current one lit) and searched, the least window, Claude's preset saved (the status saying so)
  and its version saved with PROJECTS open, PAPER;
  `gui.midi*.png`: a dial's menu over it (and once it has learned), a dial learning, one a MIDI control moves (its
  mark, the footer naming the control; both themes), a `.gui` file's knobs):
  look at both products' PNGs after any editor change.
- An example added or changed is checked by its product's `fminus.examples` (compiles with no message, plays, the same
  bits in blocks of 64 and 333; the synth's voices end, and a note on an instance a stolen voice left starts as on a
  fresh one: start sets everything the note needs). The first, Init, is the program a new instance plays, and
  `fminus.init` checks what it promises (the effect's input back bit for bit at its defaults, the synth's pitch and
  envelope, a stolen voice's instance starting afresh), `plugin.process` DEFAULT that a new processor plays it. A change
  to the contract (Patch.h) changes `fminus.contract`, the README's "Writing a program", the examples' comments and the
  placeholder text with it, for the kind it touches.
- Projects live in `~/Library/Audio/Presets/Funk/<name>/`, each product's own (`Source/plugin/ProjectStore`, which the
  processor uses through `EditorPort`): a project is a program and its dials kept as versions (each save one, the
  newest 100 kept), with presets (dial settings, applied by name). Probes never touch the user's: under CTest the
  folder is the probe's sandbox (`<prefix>PREFS_DIR/presets`, in `build-dev/sandbox/<key>/<probe>/`), and
  plugin.projects, gui.dump and gui.explorer point it at a temporary folder of their own
  (`projectstore::setDirectoryForTests`; its name starts with the product's prefix, as both products' probes run at
  once). The file format (`funk-playground-project`, version 1) is read by every later build: add fields, never change
  one's meaning (a version's `circuits`, the self-test's answers for its netlists, is such an added field); a project names its product and kind, and the other kind's is refused. The presets from before
  projects (`.fmpreset`) are migrated, never deleted. `ProjectStore` caches each file's listing entry and its presets by
  the file's time and size (one look a file: readdir and stat on the Mac; on Windows JUCE's listing, whose entries hold
  both; a name asked in another case than its file's finds the same file's entry by its device and inode, on Windows by
  its path case aside), and every write of the store (`write`: a version, a preset, the info, an import) and `load`
  keep there what they wrote or read, so a save of this process's makes no listing read its file again; a damaged
  file is read once while it stays as it is. The explorer's poll every 3 s (`Processor::projectsPolled`,
  `presetsPolled`: `listCached`, `presetsCached`) never reads a project's file whole on the message thread: a file the
  cache does not hold as it is now (another process wrote it, or none read it yet) is listed as it was and read by the
  store's `Reader` (one a process, its own thread, a path asked once until read; a read kept only while the file is
  still as a look before it found it, so it never replaces a write's entry), whose revision (`projectsRead`) makes the
  explorer poll again (`projectstore::parses`, `readerParses`, `listings`: plugin.projects POLL and CACHED,
  gui.explorer POLL, gui.switch POLL). The Reader moves its revision under the cache's lock with the read it keeps, and
  `Panel::syncExplorer` polls again when the revision moved while it polled, so the listing and the presets are of one
  state (else a read landing between them makes the rows twice). PROJECTS and EXPORT read the same cache
  (`projectsPolled`, `EditorPort::projectPolled`: `projectstore::detailCached`, a project's versions' numbers, times and
  notes, its presets and its info, which `Cached` keeps), never a project's file whole on the message thread: a file the
  cache lacks goes to the Reader, the card shows what the cache last held (a project never read: "Reading the
  project...", EXPORT's fields taking nothing) and reads again when `projectsRead` moves (`ProjectBrowser::poll`,
  `ExportCard::poll`, each frame; gui.projects READS, plugin.projects DETAIL). The A/B card's versions are still read
  whole. **The editor's save** (Cmd-S, PROJECTS' Save: `EditorPort::startSave`, `saving`; `Processor::takeSave`,
  `runSave`, `landSave`) takes what it keeps on the message thread (the draft, the dials, the answers, the MIDI map, the
  samples, the project open, its version and `projectOpens_`), reads and writes the file on a thread of its own
  (`saver_`), and lands at poll(): the project open made the one saved unless a project or version was opened meanwhile,
  the same version again included (`projectOpens_`, which an open, a fresh start, a session, the open one deleted and
  A/B's Keep of a version bump; the version saved all the same). The saver also finds the project saved's id for the
  chats when it is not the one known (`SaveJob::ownerId`: a version added to another project, a new project), so a
  landing reads no file however large the project. One asked for while one is in flight saves after it, with what the
  editor has then; a processor that goes with one in flight lands it and saves the one asked for meanwhile
  (`finishSave`); a factory project's copy is saved at once. `saveVersion` (Claude's projects tool, the probes) runs the
  same three parts in turn. The Panel says SAVING until `saving().done` moves, then the outcome (`Panel::syncSave`, at
  full rate meanwhile), and an open PROJECTS reads its lists again only when what it shows changed
  (`ProjectBrowser::rereadIfChanged`: a confirmation it asks for stands over lists that did not, and one withdrawn says
  the save changed them); `settle()` lands one in flight, `setSaveGate` (probes only) holds it before the store's work and
  `saveWritten` says its store's work is done. The store keeps one writer in the process (`writeMutex`, recursive:
  `update` and every write made through it, `saveVersion`, `add`, `remove`, `idOf`'s new id, made through `update` over
  the file as it is now, never the project as read, and the migration while there are presets from before projects, so
  the explorer's listing never waits for a save), so the saver and a write on the message thread never write over each
  other (plugin.projects ASYNC, plugin.chats ID, gui.dump PROJECTS). The poll waits `kPollAfterShow` after the frame that shows the explorer again
  (the Code view), and the explorer hashes what its rows show each frame (the lists by a revision a poll bumps).
  `list()` and `presetsOf` (PROJECTS, EXPORT) still read what the cache lacks, outside its lock. A listing's entry
  carries the info's category, tags and description (`Entry`, `ProjectEntry`), which PROJECTS and the explorer group
  and search by (`editor/ProjectGroups.h`: FAVOURITES, FACTORY, a category a group case aside, UNCATEGORISED last), so
  neither reads a file for them; `setCategory` writes the category alone. PROJECTS' view (the stars, the groups
  folded, the search, the tag, the order: `projectstore::View`, `EditorPort::projectsView`) is the folder's, one
  hidden file (`kViewFile`, `.projects-view.json`, never listed: no project's name starts with a dot) read again only
  when its time or size changes, so every instance shares it and a probe's folder has its own; `remove` unstars. The
  categories live in the files, never as subfolders (a project's name is its file's, unique in the one folder: the
  cache, the Reader and the migration all key on it). A preset has a category too (`Preset::category`, an added field
  written only when set; `setPresetCategory`; a preset saved over one of its name keeps it), and the cache's
  `PresetSummary` carries each preset's category and dials, so the presets in turn below read no file.
- **The presets in turn** (`EditorPort::presetCycle`, `stepPreset`; `Processor`'s "The presets in turn"): the project
  open's presets, by name (a number is an index there), as the store's cache has them (`presetsCached`: a file it lacks
  goes to the Reader), held by the processor and read again by `poll()` every `kPresetsPollMs`, at once after this
  processor changed them or the project open, and by a state load (which reads the session's project whole first,
  `restoreProjectBase`: its presets into the cache, so the host's program is the session's as the load returns even in
  a host just started, and the base the dials are compared with, its version's dials with the preset applied over
  them, so a session reopened shows its edits as it had them). The current one is the preset applied, else the
  first whose dials (every one the program has) are where the dials are (`dialsAgainst`), else none; `edited` the
  applied one's dials moved. A step applies through `applyPreset`, so the dials glide as any host gesture does and
  the edits to the one before are dropped, as the browser drops them. The top bar shows them right of the project's
  pill while a project is open (`Panel::stepPresets`, `openPresets`; [ and ] while no text field has the caret),
  their list is `editor/PresetPicker` (a popover the Panel routes events to, as a card: `dialogOpen`). **The host's
  programs** are the same list (`getNumPrograms`, `getProgramName`, `setCurrentProgram`, `getCurrentProgram`): a
  VST3 host reads the count once (JUCE's program parameter is made when the plugin is), so the VST3 reports
  `kVst3Programs` (128, those past the presets "-"), the AU and the Standalone the presets themselves (at least one);
  `getCurrentProgram` is `hostCurrent_`, the one applied or matched last, kept while the dials match none in the
  project it was set in (`hostFor_`: so a host that sets the program it saved changes nothing); another project open
  drops it, to the "-" past the presets in the VST3 (JUCE's program parameter sets a program only when it is not the
  current one, so a number kept from the project before would never apply that number's preset), to the first in the
  AU and the Standalone; and the host is told with `updateHostDisplay(programChanged)` when it or the names change
  (from `poll()`, never at a state load). `setCurrentProgram` off the message thread, and
  **MIDI's program change** on channel 1 (the synth's: `play()` stores it in an atomic, nothing more), are applied by
  `poll()`. Never read a project's file in the host's program calls. `plugin.presets` and `gui.presets` check it all. `Source/plugin/ProjectExport` writes a project
  as a source .zip (`funk-playground-source`) or a .fmpack (a 20-byte header, then the project's JSON as xz through
  macOS's libcompression, zlib elsewhere) and reads both back; the same rule holds for both formats. plugin.export
  checks them.
- **Factory projects** (`plugin/Factory.h`, `factory/<key>/`) are named "Factory: <name>" wherever a project's name
  goes (EditorPort.h's `factoryName`, `isFactoryName`, `ownName`): `tidyName` never leaves a colon, so no project on the
  Mac can be one, and a user project of the same own name stays apart. `ProjectStore` serves them by that name
  (`load`, `exists`, the presets; `factoryList`), and every write refuses one (`readOnly`: `saveVersion`, `savePreset`,
  `removePreset`, `removeVersion`, `remove`, `setInfo`, `write` itself); `list()` and the polls stay the Mac's. The
  Processor's `saveVersion` of a factory name, or of a new name while one is open, saves the user's own copy
  (`saveCopy`: `projectstore::add`, so "<name> 2" when the name is taken; one version, the factory one's presets and
  info; `SavedVersion::copied`, `name`); an export takes its own name, the folder too; the session keeps the name while
  the factory set has it (a session restored while the set lacks it plays, with no project named). The editor shows
  the own name (`ownName`) in rows, the browser lists them in their own group,
  FACTORY, on top (`editor/ProjectGroups.h`; starred, in FAVOURITES too), the explorer in FACTORY (folded at first),
  each made again when the set's generation moves (`EditorPort::factoryGeneration`; `projectsRead()` moves with it).
  The browser's field never takes a factory name over another program: a factory row chosen puts its copy's name
  there only when it is the one open, else the program open's (`ProjectBrowser::fieldFor`, `homeName`); EXPORT shows a
  factory project's info read-only and keeps nothing (`ExportCard::readOnly`). The set is compiled in once per
  product (until the next piece of the website's plan removes it); a project file's format does not change. Every
  caller takes the set as a snapshot (`factory::current()`, `find()`'s pointer keeps its snapshot alive) and holds
  it no longer than it uses it: never a reference kept past a call, since a fetched set can replace it at any time.
  Each text is one raw string literal in `FactoryProjects.h`, which
  turns off `-Woverlength-strings` around them (a text over 64 KB compiles; a configure and the import print the
  largest). `plugin.factory` checks it all (its PRIVATE the import's own shapes over every string of each), the import
  too (`scripts/import-factory.py`, run with python3 against made-up projects in a folder of its own: `--factory` and
  `--products` point it there); a probe never reads the user's projects.
- **The factory set's checks** (the website repository's `docs/factory-projects.md`, sections 4 and 5: the set fetched
  from the website in place of the one compiled in, step 2 of its section 13): `plugin/Verify.h` (`verify::sha256`,
  `hex`, `p256`, `p256Point`), JUCE-free, on each system's own library (macOS's Security.framework, an in-memory key
  that never touches the Keychain; Windows' CNG; Linux's libcrypto through EVP interfaces OpenSSL 3 and 4 both keep);
  the digest is the DSP library's SHA-256 (`dsp/Export.h`) on all three. `p256` refuses by itself, before a library
  sees them, what the libraries do not agree on: a point not 65 bytes starting 04 (a compressed or hybrid one), a
  signature not 64 bytes (DER), r or s zero or not below the order; a high s is valid. `plugin/FactoryKeys.h` holds
  the public keys by id (`f1`; a second row during a key change), each its bytes and its base64 from the website's
  `factory-keys.json`. `plugin.verify` holds them to `Tools/probes/fixtures/factory/` (its vectors, its test keys,
  each case's signature and files) and the built-in keys to their base64 and the platform's import; a key or a check
  changed on the website side means the fixtures made again there, never edited here.
- **The factory store** (`plugin/FactoryStore.h`; the website repository's `docs/factory-projects.md`, section 6): one
  `factorystore::Store` a process, held by every processor (`attach()`: the first starts it, the last one gone cancels
  and joins its thread, so nothing runs when a host unloads the plugin; never a detached thread or a static
  destructor's work). Its own thread does all its disk and network work: it reads the set on disk when it starts
  (checked exactly as a download, whatever the answer), and fetches only when an editor has opened (`editorOpened()`)
  or `refreshNow()` is called, the answer (`factory.json`, both products') is yes, `<PREFIX>FACTORY` is not `off` (the
  probes' environment sets it), and the set is missing or a day old. Callers never wait on it: `state()` (the status
  the FACTORY group will show, the failure as connection or check, the reason in words), `set()`, `answer()` copy
  what it last published. The folder is `Funk/Playground/Factory/<key>/` in the user's data folder (`%LOCALAPPDATA%`
  on Windows; a probe's sandbox's `factory/` under CTest; `setDirectoryForTests`), every file written beside its place
  and renamed in, a set's index and state under the folder's `.lock` after the state is read again (the higher
  sequence is never lost). The site is fixed (`kSite`; `<PREFIX>FACTORY_SITE` an https origin, or http to
  127.0.0.1/localhost, for a developer's `wrangler dev`): the address of a file is made from its hash, no redirect is
  followed (`claude::Transport::getLimited`, which also reads a body no further than the limit; the chat's calls are
  as they were), the request sends a `User-Agent` alone. Its checks are `scripts/factory.ts`'s, in its order, with
  its words (the private-data scan aside: the publisher's alone): a change of the format or of a check is made in both
  in one step, the fixtures made again there. Only a probe can give a store other keys: the process's store is made
  from `processConfig()` (the built-in keys) unless `useConfigForTests` says otherwise, which exists only where
  `FP_PROBES` is defined (the probe executables, `cmake/Probes.cmake`); a `Store` a probe makes itself never
  publishes. Nothing here shows yet: nothing in the plugin sets the answer or calls `editorOpened()` (the next piece's
  offer, the group's states and the switch will). `plugin.factorystore` checks it all against the fixtures.
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
  copy into `/Applications`) nor sign it. `scripts/package.sh --installer` makes the installer app's disk image instead,
  `build-release/dist/<Project>-Installer-<version>.dmg` (the app, with its payload, and a Read Me; signed, its payload
  inside out and the image notarized, only with `--sign` and `--notarize`). An agent may build it and look inside
  (`hdiutil imageinfo`; the payload in the build's app), never open or run the installer app.
- Linux: `cmake --workflow --preset linux-release-build`, then `scripts/package-linux.sh` makes
  `build-linux-release/dist/<Project>-<version>-linux-x86_64.tar.gz` (each product's VST3, Standalone folder, server
  folder, extension, icons and `product.env`, and `install.sh`, which is `scripts/linux-install.sh`; README, "Linux").
  An agent may make it and run its `install.sh` only with `HOME` (and `XDG_DATA_HOME`) pointing at a folder of its own,
  never the user's home: `plugin.linux` INSTALL runs it so.
- The presets build at `-j6` (the machine is shared). No network: JUCE, bgfx and shaderc come from `~/audio/.deps`
  (read-only, never write there) and FunkGui is cloned from `~/audio/libraries/FunkGui` at the pinned commit. FMinus is
  cloned from `~/audio/libraries/FMinus` at the pinned commit (`FP_FMINUS_SHA`; `-DFETCHCONTENT_SOURCE_DIR_FMINUS=`
  builds its working tree instead), and its LLVM, Bison and Flex are read from `~/audio/libraries/FMinus/.deps`, the
  installs FMinus's own builds made: never build them from here (FMinus's "system" LLVM mode, which `Deps.cmake` sets,
  never does).

## Layer and real-time rules

- `Source/dsp`, `Source/fsdsp` and `Source/fminus` include nothing from JUCE, FunkGui, `plugin/` or `editor/` (the test
  `lint.layers` checks; the DSP library has no JUCE include path). `Source/plugin/EditorPort.h` stays JUCE-free too: the
  editor includes it. FMinus comes in through its public headers only, `fminus/FMinus.h` and its circuit compiler's
  `fminus/Circuit.h` (in `Source/fminus/Circuits.cpp` and `Source/dsp/SelfTester.cpp` alone): its internals (`circuit/...` among them) and LLVM's
  headers appear nowhere in `Source/`. The schematic's model (`fminus/CirDoc`, `CirLayout`) includes none of them, nor
  anything that reaches them: only `fminus/CirDoc.h`, `CirLayout.h`, `Lang.h`, `dsp/Sources.h` and the standard library
  (`lint.layers`), so that every layer may use it.
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
  swap is over (and while `holdPending` does not hold it: the A/B's trim, at most a 20 ms ramp), hand back.
  run_function's copy of the state that plays is a third (`snapshot_`): the asker's `Engine::Snapshot`, claimed by an
  exchange at a block's end and answered in the asker's buffers (memcpy, nothing allocated; the asker writes them
  whole before it asks, so no page faults there), or taken back unclaimed. The audio thread never deletes a Patch.
  `Engine::setControls` and `process` allocate, lock and log nothing; `prepare()` (audio stopped) is the only place
  that sizes buffers.
- **The dials' carry** (Controls.h's `DialCarry` and `carryValue`, Compiler.h's and Engine.h's "The dials' carry",
  `Processor::apply`): the host's values are normalised, so a run whose declarations change (a range, a taper, an int's
  options) would give an old value another meaning. A dial of the name its slot had keeps its value in its units
  (clamped; an int or a bool its value's name, `panel.options`), a new one starts at its default, and so from the new
  program's first block: just before every offer the Compiler asks the processor's carrier (`carryFor`, under
  `meaningMutex_`) and the carry travels in the Patch (for the audio thread) and the status (for `apply`). The Engine
  plays a slot's carried value while `setControls`' meaning is the carry's `from` and the slot's value is still
  `fromNorm`; `apply` writes the carried values (one gesture each; a slot the host or the user moved since left as it
  is, one that plays its value already not written; only the newest carry writes, and only once), then stores
  `meaningProgram_` (release; `readParameters` and `chatDialValue` read it before the values): keep that order. A state
  load carries nothing (`compileNow(files, false)`, `adoptMeaning` after `replaceState`). The carrier never calls the
  Compiler (a refine asks under its mutex). chatDialValue says the carried value until `apply` has run, so run_program
  reports what plays. Projects and presets keep a value's name with its number (`projectstore::Dial::label`, an added
  field; a version saved again unchanged gains the names it lacked). `plugin.state` CARRY and `plugin.projects` LABEL
  check.
- **The panel's own dials** (Controls.h's `kHostControls`, `kPanelControls`, `isPanelDial`): a program has 64 dials,
  `kMaxControls`: its first 32 parameters the host's P1 to P32, the next 32 (then its circuits' dials, after the
  params as before) the panel's alone. Everything but the host treats both alike: the Engine, Patch, the carry, the
  frozen copy, the panel, Claude's tools (dial lines say "panel only"), projects and presets (by name), A/B and a
  recording (its blocks keep 64 dials). The host never sees one: no `juce::AudioProcessorParameter`, no automation, no
  `params_` entry (it holds `kHostControls`); `raw_[c]` of a panel slot points into `panelValues_`, so every reader
  stays as it was, and every write goes through `Processor::writeDial` (a host gesture, or the value stored and the host
  told its state changed: `withNonParameterStateChanged`; never from the audio thread). The session keeps all 32
  beside the APVTS's `<PARAM>`s, as it keeps every one of those (a slot the program that plays leaves unused may be
  the kept program's: an A/B's side with fewer dials), normalised in the program's controls as those are
  (`<PANEL><DIAL slot value/>`), and a state without them loads each at its program's default (written straight, not
  as a gesture). A mask of slots is 64 bits (`slotBit`): never `1u << c`. Under the code, dials that would leave the
  editor less than its 80 px floor (64 in the least window) fold into the dials' bar, as beside a netlist's drawing;
  the PANEL view has every one (its small rows closing up, `kTightRowGap`). `plugin.state` PANEL, `plugin.projects`
  PANEL, `fminus.contract` DIALS, `fminus.gui` LIMITS, `gui.dump` ROWS check.
- **MIDI learn** (`plugin/MidiLearn.h`; the synth's alone: the effect takes no MIDI, its AU type and VST3 bus say so,
  and those must not change, so its dials' right-click stays the host's menu): a mapping is a dial by name (as projects
  keep dials: `Learn::bind` follows the controls on every `apply`, a mapping with no dial of its name waits), a channel
  and a control, 7 or 14 bits; one control a dial and one dial a control (`tidy`). The audio thread only notes control
  changes (`Processor::midiControls`, in `hostBlock`, bypassed or not, only while `listening()`: atomics, no lock, no
  allocation): learning's first control and the controls seen (for a 14-bit pair), and each mapped slot's newest value.
  The message thread writes them (`pollMidi`, on a timer of its own at 60 Hz that poll() starts while there is
  anything to listen for and that stops itself when idle; the probes call it with their own clock) through the dial's
  port, as the editor's hand writes: a touch one gesture (begun by the first value that moves the dial, ended
  `kTouchMs` after its last control change, every one ended in the destructor), an int's or a bool's value on one of
  its values. So a learned control is the user's in the journal, undoable in the host, and the host's automation and
  the control take turns as for any touch (the last writer wins: a touch writes nothing until its control moves
  again). `GesturePort`'s gestures nest (the hand and a control at once: one host gesture, first begin to last end),
  and `writeDial`'s own (set_dial, the carry, a version's or an example's dials) nest in them too, through
  `beginOwnGesture`, which leaves the journal's attribution as it was: never call the parameter's begin/end directly.
  The channel mode messages are never learned, nor the racing wheel's controls (`wheelcc`) on channel 1 while the
  Standalone's wheel is on (`Learn::setWheel`, from `Processor::syncWheel`: the `DevicePort`'s wheel entry, read again
  when its serial moves; the wheel resends them on every device change); elsewhere they are learned as any
  (`refusal`, in words; learning goes on), and they still reach the Host block's wheel fields from any channel. Kept in
  the session (`<MIDI><MAP dial channel cc fine/>`; a state without it has none, so a session load replaces them) and
  with each project version (ProjectStore.h's `midi`, written only when there are some: opening a version that has some
  makes them the mappings, one with none leaves them; other mappings than the latest's make saving add a version,
  "MIDI map" in its note, unless they are those a version with none left, untouched since, and the latest has none:
  `midiOpened_`, `VersionMidi::inherited`). The editor (`Panel::dialMenu`, `syncMidi`): a dial's right-click,
  ctrl-click or screen reader's menu action (a `.gui` file's knob's, slider's, switch's or buttons', alike, never a
  dropdown's, whose menu action opens its list: `GuiView::Hooks::dialMenu`; none in the GUI sandbox) opens Learn MIDI
  CC / Stop learning, Forget, Forget every MIDI CC and the host's own menu (a host parameter's, in a host), never a
  write; `looks::midiMark` draws the dot (a control moves it) or the ring (it learns) at the cell's top right, and the
  footer's line names the control or says it is learning (and why a control was refused). Claude reads them in the
  dial lines ("moved by MIDI CC 74 on channel 1": matched by the dial's name, `View::forDial`, as a compile `apply`
  has not taken yet may have moved the slots `words` are by) and get_status's MIDI lines (a dial learning, mappings
  waiting for a dial of the listed controls): no tool's schema changes, and the guide's "projects" part (the chat's
  prompt too) gained one sentence. `plugin.midi` and `gui.midi` check.
- **Samples** (`dsp/Assets.h`, Patch.h's "Samples", `plugin/AssetFiles.h`, `plugin/ProcessorAssets.cpp`; the explorer's
  ASSETS): a program reads an audio file the user added as a table, `#pragma sample <field> "<file>"` filling a field
  whose module has `const float* data` and `int frames` (and `int channels`, `float rate`: the library's `Sample`,
  `sample.fm`), the main module's in every instance and Shared's, after the defaults and before init (`Patch::reset`);
  `data` const alone (a `float* data` is a note: the sound is one buffer the instances, `Asset::at`'s next compiles and
  the tools' renders share, and FMinus refuses a write through a const pointer).
  A file is decoded once, where it is added (the message thread for a drop or the ASSETS +, Claude's worker for
  write_file's `path`, the thread that loads a state or opens a version), never on the audio thread, and resampled
  (`assets::resample`: a Kaiser-windowed sinc, 48 zero crossings, its cutoff 0.95 of the lower Nyquist) only in
  `Patch::reset` through `Asset::at`, which keeps each rate made (compile, `Engine::prepare`, `offer` at another rate:
  never in a call). The Patch holds the sound (shared) while it lives, so the audio thread only reads it and a sample
  removed or replaced plays on until the next RUN; the pointer is no array FMinus sees, so a program reads within
  `frames` (`at`, `readAt` and `read` do; `read`'s float position only for short sounds, sample.fm's header). Every
  compile that renders a program takes its samples: the Compiler's (`setAssets`: a RUN and compileNow the set as it is
  when they start; a refine and a stop's restore their recipe's, `Recipe::assets`), every tool's Session
  (`measure::Request::assets`, which `scenario::resolve` fills from `ChatHost::chatAssets`, the set as it is: the
  editor's files'), and a render of another program its own: what plays and previous_run what they compiled with
  (`CompileStatus::assets`; `ChatHost::chatPlaying`, `chatPrevious`: compare's sides, trace's and view_panel's at), a
  version its own (`chatVersionSamples`: `assetsOfVersion`, sharing what the set holds of the same file). previous_run
  is what a run replaced that changed the files or the samples its lines read. The A/B's loudness and a capture's
  replay take the set as it is (an A/B side and a take keep none of their own): a new path that compiles a program for
  sound passes its samples. A panel's edit is one only while each sample the program read is the set's of its name
  (`Recipe::samples`, `CompileStatus::samples`). The set is immutable once made (`assets::with`, `without`,
  `renamed`): the processor replaces it whole under `assetsMutex_`, the Compiler's with it. Kept in the session as
  each file was added (`<ASSETS><ASSET name data/>`; a state without them has none), with each project version
  (ProjectStore.h's `samples`, the project's `sampleFiles` each once by hash; opening a version makes its samples the
  plugin's, they count in the edited mark and in the note) and in both exports (a .zip's `samples/`); EXAMPLES and
  RANDOM keep them. Caps (`refusal`, in words): 8 MB a file as added, 32 MB a program's in all, sixteen samples, two
  channels, 1 to 768 kHz, 60 s and 64 MB decoded each, a project's files 64 MB; a file's header checked before anything
  is read or allocated (JUCE's readers take its rate and length as given and give silence for what the bytes lack), and
  a WAV's or AIFF's sound within its bytes (a damaged or cut-short file refused). A state's or a version's second
  sample of a name takes the first's place (`assets::with`). Claude lists them in get_status's and plugin_state's
  Samples line, adds one with write_file's `path` (a property: the tool budget), and rename_file and delete_file take a
  sample's name. `assetfiles::decodes()` and `assets::resamples()` count, and `plugin.samples` holds processBlock to
  none.
- **A panel's edit** (Compiler.h's "A panel's edit", `Compiler::editPanel`; Patch.h's "The panel"): a RUN whose
  files differ from the recipe's (what plays) only in the panel's files (`isPanelFile`, Sources.h: `.gui`, `.sc`,
  `.sh`) is applied to the program that plays with no compile and nothing offered: no crossfade and no fade (the same
  samples, `plugin.reload` GUI), the dials untouched, the status the program's (`panelOnly`, `program` the same,
  `request` the RUN's, `sources` and the recipe's files the new ones, the panel's notes read again:
  `withPanelNotes`; its `carry` the program's: poll() may see the edit's status and never its compile's, and then
  `apply` writes that carry, once, and moves previous_run, as for the compile: a panel-only status is a run's unless
  its program is the one `apply` last took, `plugin.reload` SKIPPED). It is one only while a program plays (the status
  ok and of the recipe's program, not stopped, no refine held, no self-test answer waiting for a RUN because its Shared
  ticks: `fminus.circuit` REFRESH), the other files are the same in their order, a panel file changed and none was
  read by FMinus (`PanelBase::read`), both sides have the same first .gui file, and every dial means what it meant
  (type, range, default, taper; an int's option names: the carry keeps a value's name): else it compiles, as does a
  RUN that changes nothing (it starts the program afresh). `request(files, false)` never edits
  (the A/B's sides and its end, a recording's restart, a fresh start, a project version opened: `plugin.reload`
  OPEN); the processor asks for edits only while no A/B is on (`panelEdits`), never in compileNow. Displays that read
  other fields send their taps to the Engine (`Engine::setTaps`: a seqlock and a count, read at the end of the next
  block as the probes' set is, only for that program; another program reads its own), and the compiler's thread waits
  for that block (`tapsRead`, unless the audio thread is idle) before the status is out, so the panel reads its fields
  at once. The table is named on both sides (`PanelSpec::tapTable`, the edit's count; `Meters::tapTable`, each
  frame's; 0 the program's own taps), and a Panel reads a frame's fields only when both are its own
  (`ui::TapsShown`: a frame of its program but another table, its last fields held; else 0), so neither the edit's
  status reaching the editor late nor the audio thread idle shows a display another's field. `Patch::readPanel` is
  the one reader of a .gui file's panel (compile's too), and `Patch::tapAt` the one resolver of a display's source
  (`displaySource` over the `PanelBase`: a field or an array's element; one tap a display, `Tap::Type::none` for what
  the compile did not lay out, so no index shifts): keep it so. The editor says PANEL UPDATED IN N MS,
  run_program "Applied ... as the program plays", the journal a note (no dials set).
- **A/B listening** (`plugin/ProcessorAb.cpp`, `EditorPort::ab`, `editor/AbCard.h`): a side plays as a run of its
  files (`Compiler::request`), never a second Engine nor a Patch held back, and the draft stays as it is until a side
  is kept (`keepAb`: its files the draft, an open editor merging them as one undo step; a version's the project open);
  each side's own dials land by name when its compile does (`abDialsRequest_`, as a version's do). The matched
  loudness is a gain on the Engine's output only (`trimOutput`: after the recorder, before the bypass, so the meters,
  the scopes and a recording are untrimmed), never above 1, keyed by the program that plays (`Engine::playingProgram`,
  the side's request id, published before its compile is offered; a refine of a side takes its new id once poll()
  sees it), ramped over 20 ms (smootherstep, one tick a sample): the audio thread reads a small table inside a
  sequence count (`AbTrims`; moved under the read, the aim stays) and allocates nothing. Never turned up on the way: a
  program the table does not know keeps the gain that plays while it is on or ending (a side's refine, its old
  program while its next compiles, one from elsewhere until poll() ends it); a program coming that plays lower waits
  in the Engine (`Engine::holdPending`, `trimGate`) while the trim comes down to it, and a higher gain waits until the
  program going out is silent (`Engine::outgoing`); Close's and Keep's compile keeps the trims until it lands (that
  program at 1). The loudness is measured again once the dials moved and rested 0.5 s (`abFollowDials`; meanwhile it
  says it is measuring). A program that lands from anything else (a request past the newest known when it began, none
  of its own, refined or not) ends it, nothing restored, and so does a session loaded (`setStateInformation`: the A/B's
  waiting request taken back, `Compiler::withdraw`; its program untrimmed at once; poll() ends it); `endAb` puts back
  what played before. A side that stops as it plays (`abStopped`, after its first blocks: the compiler's status keeps
  the request before it, so it is told by `CompileStatus::stoppedProgram`) is said in `AbState::why`: the program the
  compiler plays again is the other side's, which then plays under its new id at its own trim (and its own dials), or
  another's, which ends it; one that stops in its first blocks "stopped as it began", the side before it playing on;
  AbMatch never measures a render that stopped (`Render::stopCall`: unmatched, saying why). Not in the session
  itself: a session saved while it is on keeps what played before it began (and those dials while each side has its
  own: `abKept_`, under `programMutex_`). Claude reads it as one line (`ChatHost::chatAb`, under its own mutex:
  get_status and plugin_state); keeping and switching are the user's.
- Nothing steps: a program swap crossfades (the effect) or fades the old program's voices over 20 ms (the synth: the
  held keys start again on the new one), a steal fades over 2 ms, a real dial glides over 20 ms as a stream, the
  bypass crossfades over 20 ms. The output does not depend on how the host splits its blocks (the examples are checked
  at 64 and 333, the synth's with a release mid-block), except where a dial moves (once a block): the synth's voices
  run in calls cut on the absolute 16-sample grid and are freed only on it (Engine::kGrid), so keep both.
- One stop a start (Engine.h, "Voices"): each start() a program's voice gets has exactly one stop() before its
  instance's next start(), whatever ends the voice (a note-off, the pedal, a retrigger, a steal, the quietest cut with
  every instance taken, process returning false while held, a drop for going non-finite, prepare()); only a program
  swap fades the old program's voices with none. A new path that frees a voice of `current_` calls `stopVoice` first,
  so a program's Shared (a key stack) stays right; `dsp.swap` STOPS checks it. A mono program's `legato()` comes between
  a start and its stop, never after the stop (`monoVoice` is never a released voice).
- **Shared on its own** (Patch.h's and Engine.h's "Shared on its own"; the synth's only): an instrument's Shared may
  have `tick(int n)`, `noteOn(float note, float velocity)` and `noteOff(float note)`, each appended as a nonblocking
  shim of its own only when `lang::sharedHooks` reads it declared so (a method of the name declared otherwise is the
  program's own, never called, with a note: a program that has one plays as it did), and params named as dials, which
  `reset()` sets to the dials' defaults before Shared's init (init runs at compile, before any value is known) and the
  Engine to the dials' values at the sample (`setSharedDials`; the snap's for a program that came this block) before
  each tick and each note hook. The tick runs at each absolute multiple of `kGrid` at the start of the part that begins
  there (after the events at that sample, before the voices), never per part: so it moves the same at any block size,
  and trace's calls of one sample give pass 1's bits. The note hooks come before the voice's start and stop;
  `sharedKeys_` makes each noteOn get one noteOff before that key's next (a retrigger: noteOff, then noteOn; the pedal's
  notes at its going up; All Notes Off and `prepare()`: every note on, before the voices' stops); a swap starts the new
  program's keys empty (the held keys' restarts give the noteOns) and keeps the old one's for a revert, which takes them
  up again (those let go meanwhile get their noteOff), and the old one's Shared hears nothing more while it goes but the
  noteOff of a key let go under the pedal before the swap, before the pedal's going up stops that voice
  (`oldSharedKeys_`). A new path that ends a note calls `sharedNoteOff` before the voice's stop. The frozen copy looks
  the hooks up (`freeze`), and a hook that runs past its budget stops the Patch as any call does (`Method::sharedTick`,
  ...). A program with none of them gets no call and no write. A program whose Shared ticks is never refined (below, the
  self-test: its answer waits for the next RUN). A recording whose program's Shared ticks never replays from a fresh
  state (its tick ran before the take): CaptureReplay says so. `dsp.shared` checks it all.
- **Mono** (Patch.h's and Engine.h's "Mono"; the synth's only): `#pragma mono [dial]`, read by Patch's compile from the
  files FMinus read as it read them (`Patch::mono`, `monoDial`: a bool or int dial switches it, read by the Engine at
  each note-on and note-off with `dialAt`), and Voice's optional `legato(float note, float velocity)`, appended as a
  nonblocking shim only to a mono program and only when `lang::legatoHook` reads it declared so (else the program's own,
  with a note). The Engine keeps the held keys' press order (`pressed_`): a note while the newest voice not released
  sounds goes to it by `legato` (`playNote`), else a voice of its own (`startVoice`, the voices' rules); every other
  voice of the program then fades as a steal's (`fadeOthers`, uncounted); the key it sounds let go returns it to the
  newest key still held (`newestHeld`); the held keys started again on a new program (or a revert) are `restartHeld`'s
  (mono: Shared's noteOns in press order, one voice). The player's note-on is `noteOn` (it stamps the press), a held
  key's restart `playNote` (it does not). Shared's hooks are per key, mono or not. The frozen copy looks `legato` up,
  and one past its budget stops the Patch (`Method::legato`). The mono dial moving is the "once a block" exception of
  block-size invariance; the rest is invariant. `dsp.mono` checks it all.
- The circuit dials (Patch.h's "Circuits", Engine.h's "Circuit dials"): a circuit's pot or switch that is a dial reaches
  its module through `set()`, which the Engine calls through the program's shim (`Patch::setCircuits`: no allocation,
  the values into a buffer the compile sized) on the absolute 16-sample grid, with the glided values at that sample,
  only while a bound dial glides, through the tick at or after it lands, and for ceil(0.02 rate / 16) + 1 ticks after
  a switch changes (FMinus ramps a switch a step a call over 20 ms); and before an instance's first process or start (a
  new program's; an instrument's voice whose instance missed calls while free: `Engine::instanceGen_`, the fed pattern)
  once, or with a switch bound as many times in a row as its ramp takes, so it starts at the dial's position. Never while they stand still (`Engine::circuitCalls`, `fminus.circuit` DIALS and SWITCH). A named exception
  to the effect's calls: while a circuit dial moves, the effect's `process` is cut at those grid ticks, and then only,
  so a program that keeps state per call may differ there, as it may where any dial moves; the synth's parts are cut on
  the grid already. FMinus's `clear()` keeps each switch where `set()` left it (a guard's reset, a voice's start from
  silence), which the dials rely on: no call follows a clear.
- Frozen dials (Patch.h, Engine.h, Compiler.h): the Compiler's freezer thread compiles the playing program again with
  its dials as FMinus frozen constants once `Engine::dials` says they have held still for 3 s (from the recipe the
  compile kept, `Patch::Recipe`), and offers it (`Engine::offerFrozen`); the audio thread plays it only if it is of the
  program that plays at exactly the dials' values, through the same instances (`Patch::useFrozen`), and drops it the
  block a dial moves; copies go back through four retired slots for `collect`. The copy compiles in FMinus's Math::Fast
  (the user's choice, 2026-10-27, and again 2026-10-29 after a day in exact math: faster on average, the same sound), so
  it is not the same bits as the program's Math::Vector: its last bits move, and while it plays the output may depend on
  where the host's blocks end (while a recording is armed none plays: `Engine::setOwnCode`). Never add fast-math flags
  or anything else that changes more than a math function's last bits. `dsp.freeze` (its own program: the same samples,
  finite and the difference 80 dB under the signal; the examples, and in LONG 64 RANDOM programs for ten seconds: the
  same spectrum, every octave band, level and peak within 0.1 dB, since an FM voice's phase drifts an ulp at a time) and
  plugin.process FREEZE check it; `dsp.freeze` also times a copy in exact math (`Patch::freeze`'s `exactMath`, measured
  only: since FMinus c75caab it plays the program's bits, at 94% and 93% of the program's time to the fast copy's 92%
  and 90%, README "Frozen dials"); a probe that compares two processors' bits must not run long enough to freeze (3 s of
  still dials), or turns the freezer off (`Compiler::setFreezing`).
- The circuits' self-test (`dsp/SelfTester.h`, Compiler.h's "The self-test"; `fminus/Circuits.h`): a netlist that
  leaves its configuration open plays a provisional one, and the jobs of a compile that plays (RUN's, `compileNow`'s,
  a refine's; never a check's, a failed compile's, Measure's or another tool's) go to the Compiler's `SelfTester`,
  which runs FMinus's `selfTest` on threads of its own (a coordinator, and min(4, max(1, cores - 2)) workers made and
  joined for each test, at the utility QoS class), never on the audio thread, the message thread or the compiler's,
  and never holding the Compiler's mutex. RUN and the audio thread never wait for it; a test the newest program no
  longer wants is cancelled and stores nothing; two instances never test one key (`Answers::claim`). Once an answer
  lands for a key the program that plays used provisionally, the compiler's thread refines, at a pass of its own (never
  the one whose compile made it due): it compiles `recipe_.files`, what plays (never the draft, never a failed RUN's
  `status_.sources`), under a new program id and offers it as any program; it waits while a RUN is queued (which reads
  the store itself, and the refine is dropped), while a recording is armed (`Engine::ownCode`), and in the synth until
  `Meters` shows no voice sounding, fading or held (no held note restarts), never for a program whose Shared ticks
  (`Recipe::sharedTick`: its state moves between notes, and a new program's Shared starts afresh; the answer plays from
  the next RUN, which reads the store), and it looks again just before the offer
  (a refine that may not play then is held, `held_`, and offered later). A refine answers no request: its status keeps
  the request before it (`request`, `ok`, a failed RUN's `errors` and `sources`: the editor and Claude still see why the
  draft fails), the refined program in `CompileStatus::program` (what `Meters::program` names; `Processor::apply` and
  the panel read it, never `request`) and `refined` set; when that request compiled, the refine's notes, ending with
  `circuits::chosenNote` (and the test's own warnings). A test that gives no answer (uncancelled: nothing finite under
  the cost cap, the budget spent, an internal error) reports its `Outcome`, and the key is never tested again in the
  process (`Answers::claim` refuses it); the compiler's thread then brings the status up to date (`checkOutcomes`: a new
  revision, the same program), `circuits::gaveUpNote` in place of the provisional note, and the provisional
  configuration plays on (`Compiler::setSelfTestBudget`, probes only, makes one give up). One compile offers at a time: `compileNow` waits while the compiler's thread compiles, and the
  thread starts no RUN while `compileNow` (or `selfTestNow`'s refine) does. The answers are kept with the session
  (`<PROGRAM><CIRCUITS><ANSWER key text/>`) and with each project version (its `circuits`), and stored again
  (`circuits::restore`) before that program compiles. A recording never reads the store: each program the capture
  notes keeps the configuration each of its netlists was generated with (`CompileStatus::circuits`, Sources.h's
  `CircuitPins`, from `Patch::Recipe::pins`), and its replay and trace's "capture" compile with them
  (`Patch::compile`'s and `measure::Session`'s `pins`), so a provisional take replays provisional after the answer
  lands. A probe that wants no test in the background turns them off
  (`Compiler::setSelfTesting(false)`) and runs them with `selfTestNow` (the jobs waiting, then the refine); a probe's
  netlists whose configuration is open are `Tools/probes/common/AutoClipper.h`'s.
- The schematic's live probes (Engine.h's "Probes", Patch.h's, Circuits.h's): every netlist is generated with FMinus's
  probe fields (`Options::probes`: the same output bits, messages and self-test key), so the program, its frozen copy
  and every recompile share one layout and choosing a probe never compiles or swaps anything. The editor's set
  (`ProbeSet`: a program id, a circuit field, an element, four nodes) goes through the processor
  (`EditorPort::addProbeOwner`, `setProbes`, `removeProbeOwner`: the newest owner whose set is armed; a picture's
  Panel owns none) to `Engine::setProbes`, a seqlock and a generation; the audio thread applies it at a block's start
  only to current_ of that program id (the synth's newest sounding voice, the one before disarmed), never writes into
  another Patch (a swap and prepare() forget the target), and after each whole call (the followed voice's part) copies
  the newest samples (at most 1,024; a longer call starts the run `Engine::probe` reports again, so no scope joins two
  calls) into its rings of atomics and reads and zeroes the stats into `Meters::probes`: no allocation, no lock. **No
  call is ever cut for a probe**: the output's bits are the same probed or not (`dsp.meters` PROBES). Their cost stays
  with whoever looks: with nothing armed, a generation load a block (and the set's few words in the meters; nothing
  computed) and the module's one branch a sample; a Panel arms only while its Schematic view shows a net that
  resolves, and clears its set when the view hides, the tab changes or it goes.
- The displays read what the audio thread publishes, without a lock: after each block the Engine measures the
  input (the effect's) and the output and reads the displays' fields (at most `kMaxDisplays`, 32: the Panel's masks of
  displays hold a bit each, so 32 is their ceiling; a shader's 16 inputs, `kMaxShaderInputs`, are FunkGui's 16
  `u_params`; `Patch::tap`, a field of the effect's instance,
  or of the synth's newest sounding voice, or of Shared, `shared->x`, from its one Shared whether a voice sounds or
  not; after a panel's edit that reads other fields, `Engine::setTaps`' table for that program, through `Patch::tapOf`) into `Engine::Meters`, through an `fsdsp::Seqlock`, and writes the scope's
  rings of atomics; the editor reads them each frame (`Panel::tick`: each ring once for all its displays, a spectrum's
  FFT only when the ring's samples changed, `DisplayFeed`). It measures the levels and writes the rings only
  while someone reads them (`Engine::readers`: each Panel while the host ticks it, `EditorPort::addMeterReader`, from its
  making, letting go after five idle calls and a second with no tick (a minimised or hidden window: EditorHost skips
  its frames, not its idle calls) and taking it again at its next tick, Panel.h "The meters' reader", gui.panel HIDDEN;
  a tool that listens adds itself, `Processor::addMeterReader`, and removes itself after; each side's peak and energy
  in lanes, `dsp/Levels.h`, dsp.meters LANES): with no reader the levels
  read 0 and the rings stand still, the fields go on (the shift lights' field too: `Meters::lights`, which the
  Standalone's wheel reads), and a headless Panel with no other reader (view_panel's picture)
  waits for two blocks measured. `Meters::program` names the program the fields
  are of (`Patch::program`, the compile's request id): the panel shows them only on that program's displays, since
  the audio thread takes a new program at its next block. Nothing there allocates; `dsp.meters` checks the values.
- **What a tool that listens reads** (the MCP's listen, `plugin/Live.cpp`): `Meters` also carries, every block and
  whoever reads, the host block the programs read (the transport, an instrument's wheels and pedal), the samples
  counted, an instrument's voices (sounding, released, held by the pedal, fading), the keys held, the player's
  note-ons (`Engine::heard`, in the event loops only: the held keys a new program starts are none), the steals (set on
  the steal paths in `noteOn`) and the last voice dropped for going non-finite (its key and sample), whether the program
  plays mono at the block's end (`Meters::mono`, `Engine::monoNow`: listen's voices "of 1: mono", measure_output's CPU
  line "mono: one voice at a time", never sixteen; `dsp.mono` SWITCH); counts run on from
  the Engine's making, so readers take differences. `Engine::blocks` is a ring of atomics of each block's cost and
  context (`BlockStat`, written after the load measurement). The processor keeps the host's bypass, its largest block
  and its block count in atomics (`hostBlock`). Scalar stores only: no lock, no allocation. Readers never call
  `Engine::takeAlerts` (it clears the latch `poll` reads): they read the latched `chatAlerts`.
- The output guard (Engine::guard) stays: whatever a program computes, what leaves the plugin is finite and within
  +-4. Hosts and speakers come first.
- **The loop budget** (Patch.h's "The loop budget", Engine.h's and Compiler.h's "A program that stops"): every compile
  that plays (compile()'s second pass, freeze(), ir(), frozenIr()) has FMinus's `Options::loopBudget`, the frozen copy
  too (it costs 0.2% to 2.7%: `dsp.budget`, README "A loop that never ends"); each call into a program goes through
  Patch, which sets its budget (`budgetOf`: 2^32 a second of its samples, 16 samples' worth at least; init and end
  2^32) and after it records a stop (`Patch::stopped`), and calls nothing of a stopped Patch until reset() (its state
  may be any), not even an end: reset() and the destructor call none (a leak of what init allocated, never a double
  free). A new call path into a program goes through Patch too, never around it. The Engine fades a stopped
  program out over 20 ms from its last samples (`Exit`, `exitVoice`; the effect keeps each block's input for it),
  starts no note on it, goes back to the program it replaced while that one is still there and the program was offered
  `revertible` (the Compiler's RUN, until `settle()`: one exchange of `revertible_` decides who wins), and publishes
  the stop (`stops()`, `lastStop()`); an instrument's revert after the block's glides goes on from the dials the old
  program's instances held since the swap (`heldDials_`), never advances a glide twice; a RUN's land() waits for the
  program's first blocks (a block under way counts as busy, up to the time the largest call's budget takes at 256
  cycles an iteration: `firstBlocks`), so a stop there fails the RUN as a compile does; a later stop is the
  compiler thread's (`checkStops`: the status, `stopped`, and `previous_`, the last program of other files that
  played, compiled again with its pins and played). A program that stopped is never gone back to, refined or frozen.
  `dsp.budget` checks it all.
- **The bounds checks** (Patch.h's "Bounds checks"): every compile that plays as the program is written (compile()'s
  second pass: RUN's, a refine's, every tool's render; and ir()) has FMinus's `Options::boundsCheck`; the frozen copy
  (freeze(), frozenIr()) never has (`frozenOptions`: the build that plays at full speed, a compile the freezer makes
  anyway). After each call Patch reads FMinus's record (`within`, `outOfBounds`: a relaxed load, nothing allocated) and
  a failed check stops the Patch as a loop past its budget does, the check copied into `Patch::bounds()` (plain data,
  its texts cut to fixed arrays) and published with the stop (`Engine::Stopped::bounds`), so the Compiler, Measure,
  trace and view_sound word it at the access (`stopMessage`, `stopWhy`; `CompileStatus::bounds`, the editor's status
  line); reset() empties the record (`clearBoundsViolation`) before any init, since a call while it holds a check
  returns partway. A pointer the program keeps (a heap block, a host's buffer) is never checked: an example or a
  library file whose buffer fits a fixed array keeps it in one (the Echo's and the Pluck's, sized for 192 kHz; the
  library's delay, reverb and wavetable stay on the heap, the guide's BUILDING A LARGE PROGRAM says so). `dsp.bounds`
  checks it all, and that a moved buffer plays the same bits.
- The processor copes with what a host may do: the bypass crossfades between the Engine's output and the input (the
  effect) or silence (the synth), and the Engine keeps running (the synth's on the note-offs only, so a note let go
  during the bypass releases); a block before any `prepareToPlay` passes the input through, sanitised; a host's NaN
  keeps the last good value (and its text is a dash); a state value that is not a number loads as 0.5.
  `plugin.process` and `plugin.state` check each.
- The Host block (host.fm: `kHostFile` in `HostFile.cpp`, the one file the plugin provides; `Patch::hostSource` puts
  it in the system prompt): the processor reads the playhead once a block into `Engine::setTransport` (none: the
  Engine's own clock at 120 BPM, not playing); the Engine fills the block before every call part (`syncHost`, the
  position at the part's first sample). The synth also reads MIDI's CC 1, CC 64, pitch bend, channel pressure and a
  racing wheel's CCs 16 to 24 and 48 to 51 (`dsp/Events.h`'s `wheelcc`, `Engine::wheel`: an MSB resets its LSB; at
  rest after `restart`) into events at their offsets, its keyboard's wheels come through `EditorPort::playWheel` and
  the processor's lock-free ring of UI events, and the Engine holds the sustain pedal's notes itself (`Voice::pedal`);
  the effect's wheels stay at rest. A new field: kHostFile, `HostBlock`, `HostLayout` and `setHost` together; `dsp.host`
  checks. host.fm's text is in the guide's start part, whose token bound (`plugin.mcp`) has little room: say a new
  field in as few words as it takes.
- Flush-to-zero: the programs compile for it (`Denormals::Flush`), and the plugin runs them in it on every thread:
  `Engine::process` opens an `fsdsp::ScopedFtz` (the audio thread has it already; offline renders, Measure and the
  chat's tools, would otherwise pay FMinus's wrapper two FPCR writes a call), and so do `Patch::reset` and its end
  (a program's init and end). The dials reach an instance through slots looked up at compile (`Patch::setValue`,
  `setStream`: inline stores, as FMinus's own setters write them), and the Engine writes them only when they change
  (`Engine::feedGen_`): keep both when the dials' path changes.
- Flags: `-ffp-contract=off`, never `-ffast-math`. Product constants come from the generated `Product.h`, never from
  `JucePlugin_*`. The editor never uses JUCE's parameter attachments: widgets write through FunkGui's
  `GestureController` to the ports (`funkgui::JuceParamPort`).

## Windows

- **What builds.** On Windows 10 1803+ / 11, x64, with clang-cl (FMinus's compiler there; the `windows-*` presets, from
  an x64 Native Tools prompt): each product's VST3, Standalone, MCP server and extension (`"platforms": ["win32"]`,
  `server/<Name>MCP.exe`), the sandbox's helper, the installer ("The installer" below) and the probes. No AU, no signing, no export list (a DLL exports what is marked dllexport: JUCE's entry points); `lint.exports` and
  `lint.signature` are macOS's. `FP_WINDOWS` and `FP_WITH_GPU` (CMakeLists.txt) choose; `FP_WINDOWS_LIBS` (Winsock,
  CNG, the shell) are linked where the code calls them. `-Werror` and LTO start off there; turn them on once a
  Windows build is clean. RelWithDebInfo inlines fully there (`/Ob2`, not CMake's `/Ob1`), as `-O2` does on macOS.
- **The words there.** What the panel, the chat and the prompt say of the machine and its keys comes from
  `plugin/EditorPort.h`: `kThisMachine` ("this Mac", "this computer") and `shortcutText` (a text's "Cmd-" is "Ctrl-"
  on Windows, whose editor host gives Ctrl the editor's Cmd); the examples' "Cmd-Return" is "Ctrl-Return" as CMake
  writes Examples.h there. A new user-facing "this Mac" or "Cmd-" goes through them.
- **The probes there** run with twice their timeouts (`FP_PROBE_TIMEOUT_SCALE`, cmake/Probes.cmake), and one that
  crashes says where (`ProbeMain.cpp`: stdout and stderr unbuffered; heap corruption, an access violation, the C
  runtime's invalid-parameter check, abort and terminate print the stack on stderr, named from the PDB). Windows' heap
  finds what macOS's rounding hides: a probe writes only within the buffers it sized (the last block of a render is
  what is left, never a whole block).
- **The GPU layer.** FunkGui (since v0.24.0) draws on Direct3D 11 there (its render view a click-through child window of
  JUCE's peer; WARP, Windows' own rasteriser, where no GPU starts), so the editor is `gpu/CreateEditorGpu.cpp`'s
  EditorHost as on macOS, and view_panel's pictures are GPU pictures (bgfx headless). Its shaders and the plugin's
  (a .gui file's) are compiled by a `shaderc.exe` built from bgfx.cmake (unless the deps cache holds a stamped one),
  HLSL through the system's `d3dcompiler_47.dll` (`windows`, `s_5_0`: ShaderTools' defaults there); CMakeLists.txt
  copies it into the VST3's `Contents/Resources` and beside the Standalone's `.exe`, where Processor.cpp's
  `shaderTools` and the sandbox's helper look. Shaders.cpp starts it with its two handles alone (its log and NUL:
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, as the MCP server starts its helper): the C runtime's files are inheritable,
  and one a shaderc inherited (another thread's cache entry on its way, a host's file) could be neither renamed nor
  deleted until it exited (`gui.shaders`' `cache.held_none_stuck`). bgfx.cmake comes from the deps cache, else GitHub,
  as on macOS. With
  `FP_WITH_GPU` off neither bgfx nor shaderc is fetched or built (FunkGui::core and the harness only), the editor runs
  in `editor/nogpu/SoftEditorHost` (EditorHost's input, zoom, resizing, menus and chooser rules: change them together;
  it draws through SoftRaster only when the frame's primitives change), Picture.cpp's pictures take SoftRaster's path
  (`FUNKGUI_HAS_BGFX` unset), a shader draws as its placeholder, and `gui.shaders` is left out.
- **Platform code is named for its platform**: `Source/plugin/standalone/*Win.cpp` (Windows' title bar, a JUCE menu bar
  in the window: `NativeWindow.h`'s `menuBarModel()`) and `*None.cpp` (no racing wheel) beside the macOS `.mm` and IOKit
  sources, chosen by CMakeLists.txt's filter; elsewhere `#if defined(_WIN32)` beside the POSIX code (SelfTester's thread
  priority, Measure's CPU clock (`QueryThreadCycleTime` over the measured TSC rate, `profile::tscPerMs`), Profile's
  sampler (`SuspendThread`, a high-resolution waitable timer, no stack walked), Shaders' process and temp folder,
  ChatStore's crypto (CNG: the same FPK1 file), Patch's aligned memory, the projects' folder (`Documents\Funk\<name>`),
  the chat's (`%APPDATA%\Funk\Playground`)). Include `windows.h` only in a `.cpp` (or `mcp/McpProtocol.h`, which
  undefines its `near` and `far`), after `NOMINMAX=` and `WIN32_LEAN_AND_MEAN=1` (global definitions, JUCE's own
  values).
- **The MCP sockets on Windows** are Winsock's AF_UNIX, reached only through `mcp/McpProtocol.h`'s platform functions
  (`PollFd`, `pollFds`, `recvSome`, `sendSome`, `closeFd`, `acceptOn`, `socketPair`, `peerAlive`, ...): a socket is an
  int on both. What is no socket there (the server's stdin, the sandbox's helper's pipes) is bridged to one by a thread
  of its own (`bridgeFrom`, `bridgeTo`, `stdinFd`), so the server stays one `poll()` loop and never blocks; the helper
  is started with only its two pipes and stderr inherited (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`), as
  `POSIX_SPAWN_CLOEXEC_DEFAULT` does on macOS. `plugin.mcp` and `plugin.sandbox` drive POSIX child processes and are
  left out of the Windows probes; `Tools/probes/common/win/unistd.h` gives the probes their few POSIX calls (getpid,
  setenv), on the probes' include path alone, never a plugin target's.
- **The DSP core's x86 bodies.** fsdsp's `Simd.h` (the u32x4 ops), `BusSum.h` and `FastMath.h` (`signMask`) carry
  SSE4.1 / AVX2 + FMA bodies beside NEON's, added here for Windows (their SEED.tsv hashes no longer match: carry the
  change to FSynth, then sync). The x86 build compiles with `-mavx2 -mfma` (`cmake/Flags.cmake`).

## Linux

- **What builds.** On Linux x86-64 (a CPU with AVX2 and FMA, as on Windows), with clang and lld (the `linux-*` presets):
  each product's VST3 (`<Name>.vst3/Contents/x86_64-linux/<Name>.so`), Standalone, MCP server and extension
  (`"platforms": ["linux"]`), the sandbox's helper, the headless renderer and the probes. No AU, no installer app
  (CMakeLists.txt calls no `fp_add_installer` there: its Engine has macOS and Windows bodies) but a package
  (`scripts/package-linux.sh`: a `.tar.gz` whose `install.sh`, `scripts/linux-install.sh`, installs for the user alone,
  below), nothing signed.
  `FP_LINUX` (CMakeLists.txt) chooses; `-Werror` and LTO are on, as on macOS; every static library is
  position-independent (`CMAKE_POSITION_INDEPENDENT_CODE`), FMinus's, FunkGui's and bgfx's too, since each ends up in
  the VST3's shared object. `FP_LINUX_LIBS` (OpenSSL's libcrypto, libcurl) is linked where the code calls it. Clang
  refuses JUCE 8.0.4's `AudioPluginInstance` layout constructor: `cmake/Deps.cmake` adds `-fdelayed-template-parsing`,
  as FunkGui does.
- **Exports.** The VST3 exports `GetPluginFactory`, `ModuleEntry` and `ModuleExit` and nothing else, through a version
  script (`<build>/exports/<key>-vst3.map`): no symbol of LLVM's is bound to another loaded object's, nor another's to
  ours. `lint.exports` checks the built `.so` (`nm -D --defined-only`).
- **The GPU layer.** FunkGui draws with bgfx on Vulkan, in an X11 child window of JUCE's peer (XWayland on a Wayland
  desktop; Mesa's lavapipe where no GPU driver is), so the editor is `gpu/CreateEditorGpu.cpp`'s EditorHost as on macOS.
  Its shaders and a `.gui` file's are SPIR-V, compiled by the Linux shaderc the deps cache holds stamped (else built
  from bgfx.cmake); CMakeLists.txt copies it into the VST3's `Contents/Resources` and beside the Standalone's
  executable, where `bundledShaderc` (`Processor.h`) looks. The Standalone's own file is `/proc/self/exe`'s there:
  JUCE's `currentExecutableFile` on Linux is dladdr's name, which for a program is its argv[0] as typed, so a
  Standalone started by name through `~/.local/bin` would look in the working folder (`plugin.linux` STANDALONE).
  With `FP_WITH_GPU` off, `editor/nogpu/SoftEditorHost` as on Windows.
- **Platform code** takes the POSIX branches macOS takes, and the other side of `#if defined(__APPLE__)` where a pair
  names the Mac alone: the Standalone takes Windows' `NativeWindowWin.cpp` (JUCE's window and title bar, a JUCE menu bar
  in the window: nothing in it is Windows') and its own `RacingWheelLinux.cpp` (below); `ChatStore`'s crypto is
  OpenSSL's libcrypto (AES-256-CBC, PBKDF2 and HMAC with SHA-256, `RAND_bytes`: the same FPK1 file), sealed with the
  machine's key (`chatstore::machineKey`: `/etc/machine-id`, else D-Bus's, else the host's name; never JUCE's device id,
  which starts `cat` and `lscpu` there) and the account's name from the password database; `kThisMachine` is "this computer" and
  `shortcutText` gives "Ctrl-" (JUCE's command key is Ctrl there), and the examples' "Cmd-Return" is "Ctrl-Return" as
  CMake writes Examples.h; the profile has no sampler (`profile::available()` is false), `FolderSync` no watcher (it
  looks every half second), the chat's pictures are JUCE's decoders' (PNG, JPEG, GIF), and an `.fmpack` is written with
  zlib (an xz one from macOS is refused). The MCP server starts the sandbox's helper with its two pipes and stderr alone
  (glibc's `posix_spawn_file_actions_addclosefrom_np`, as `POSIX_SPAWN_CLOEXEC_DEFAULT` does on macOS).
- **The chat's HTTPS** is libcurl's (`ClaudeApi.cpp`'s Linux `HttpsTransport`, `CURL::libcurl` in `FP_LINUX_LIBS`):
  JUCE's own Linux `WebInputStream` speaks plain `http://` alone, and its curl client fills a whole read before it
  returns, so `JUCE_USE_CURL` stays 0 on every target. It keeps the transport's rules (no redirect followed, a 200's
  body to `onChunk` as it arrives, `cancel` within a 40 ms poll, two minutes idle at most); `plugin.providers`
  TRANSPORT checks them against the probe's own server.
- **The user's folders are XDG's** (`fminus/Xdg.h`, plain C++ in the lowest layer for every side): the projects
  `$XDG_DATA_HOME/Funk/<name>` and the chat's store `$XDG_DATA_HOME/Funk/Playground` (`~/.local/share` unset or
  relative), the shaders' cache `$XDG_CACHE_HOME/Funk/<name>/Shaders`, a shader build's temporary folder
  (`shaderTempBase`) `$TMPDIR`, else `$XDG_RUNTIME_DIR`, else `/tmp`; `$XDG_RUNTIME_DIR` is taken only when it is a real
  folder of the user's with no group or other bits. The MCP sockets' folder is `$XDG_RUNTIME_DIR/Funk-<name>-mcp`,
  else `$TMPDIR`'s (else `/tmp`'s) `funk-<uid>/`, made 0700 and refused (no MCP) when it is a link or others may write
  into it. A new store or cache on Linux goes through `xdg::` too; a probe points the variables at its own folders.
- **The chat's keys go to the desktop's keyring** (`plugin/SecretStore.h`: the Secret Service through libsecret,
  loaded at run time, never linked) when a session bus and libsecret are there, else to their FPK1 files. A key's file
  exists only when its last save did not reach the keyring (none ran, or it refused: left locked), so it is newer than
  the keyring's item and is read first, then moves in when the keyring takes it. `hasKey` asks the keyring without
  unlocking (no prompt), at most every 3 s; `loadKey`, `saveKey` and `forgetKey` may have the desktop prompt to unlock
  it and wait for the answer, so the agent reads a key with its mutex released (`ensureKey`, `otherKeyOf`) and
  `chatState` asks `hasKey` before taking it. libsecret's lookup and clear say nothing about a keyring left locked (a
  lookup finds nothing, a clear removes nothing): `readKey` asks `has()` after an empty lookup (the key kept, said
  locked), `SecretStore`'s clear unlocks first, and `dropKey` asks `has()` after it (a forget that left the key false,
  with why, and the chat says it was not removed). Never the keyring while
  the store's folder is a test's or a probe's sandbox: a probe gives a `secrets::Store` of its own
  (`setSecretStoreForTests`), or the desktop's with `DBUS_SESSION_BUS_ADDRESS` pointing nowhere (libsecret's calls fail
  at the connection), never the user's bus.
- **The racing wheel is the kernel's** (`plugin/WheelEvdev.h`, `standalone/RacingWheelLinux.cpp`): hid-lg4ff switches
  and drives the G29; the app reads its evdev node and commands it through sysfs and force feedback (README, "A racing
  wheel", with the udev rule that gives the user `range`, `alternate_modes` and the LED files). `wheel-dump` is
  macOS's; `evtest` is the Linux tool.
- **Never a shell between the plugin and a path.** JUCE's `Process::openDocument` and `File::startAsProcess` run
  `/bin/sh -c "xdg-open \"<path>\" || ..."` on Linux, where a folder named with `$(...)` or a backtick (a project's
  name) would run, and leave a zombie in the host. Start a program through `plugin/Launch.h` instead
  (`launch::detached`: an argument list, a double fork, no descriptor of the host's kept; `openerArgs`: xdg-open);
  `plugin.folder` LAUNCH checks it.
- **Types.** `int64_t` is `long` on Linux (`long long` on macOS and Windows), so a `juce::var` converts to it
  ambiguously: read a number from one as `juce::int64` (`static_cast<juce::int64>(v)`), and print a 64-bit value with a
  cast to `long long` or `unsigned long long` for `%lld` and `%llu`, as the code does everywhere.
- **The probes there** are every probe, `plugin.mcp` and `plugin.sandbox` too (POSIX child processes; `plugin.sandbox`
  finds the helper's processes through `/proc`), `plugin.linux` (Linux's alone: the folders, the MCP folder, the
  keyring, the wheel over evdev, the package's install script), and `lint.exports` and `lint.layers`; every one passes,
  in an amd64 Arch container on Apple silicon too (Rosetta, Mesa's lavapipe, Xvfb with openbox).
- **A row a machine cannot judge is skipped, and says so**, never left out: `probe::skip(P, key, why)`
  (`Tools/probes/common/Probe.h`) prints `SKIP       <key>  <why>` where its SPEC line would be and adds the note
  `skip_<key>` (every character of the key outside `a-z0-9` an underscore: `skip_picture_gpu`) to the probe's results
  JSON (`<build>/probe-results/<key>/<probe>.json`); the rows that can be judged there run in its place. A row of a
  feature macOS alone has is no `#if defined(__APPLE__)` without an `#else`: the other platforms' behaviour is checked
  there (`plugin.export`'s `package.zlib` and `refuse.xz`, `plugin.folder`'s `watch.watching` and `watch.poll_s`,
  `plugin.chat`'s `pictures.tiff_refused`), or, with none to check, the row is skipped saying so (`plugin.chat`'s
  `pictures.upright`: ImageIO's EXIF orientation). Each kind of skip has its helper, which a new row of the kind takes
  too:
  - **a GPU picture's pixels** (`Tools/probes/common/Pictures.h`: `noGpuPictures()`, the words a picture's text gives,
    and `noGpuPicturesSkip()`, the skip's): FunkGui reads no frame back from Vulkan (`gpuPicturesSupported()` false),
    so view_panel's, the window's and the sandbox's pictures are SoftRaster's on Linux, each shader its striped
    placeholder. `gui.shaders` (PICTURE's GPU rows; TEXT's text, numbers and ticks) and `plugin.sandbox` (SHADER's
    colours) skip those; `picture.soft_said`, `picture.not_held`, `picture.panel_soft_said`, `shader.soft_said`,
    `shader.placeholder`, and `plugin.chat`'s `view.gpu`, `plugin.mcp`'s `call.picture_gpu` and `plugin.sandbox`'s
    `render.gpu` check the fallback and its words (`Drawn without the GPU (no GPU pictures on this platform)`), as a
    build with `FP_WITH_GPU` off checks its own;
  - **a time budget under emulation** (`probe::emulated()`: an x86-64 build run by Rosetta, whose `/proc/cpuinfo`
    names the CPU `VirtualApple`): `dsp.tune`'s `long.seconds` and `long.cancel_at_once`, `plugin.capture`'s
    `size.status_ms` and `size.stop_ms`. On a machine of its own they run;
  - **a JUCE window with no X display** (`probe::noDisplay()`: `$DISPLAY` unset on Linux, where JUCE's X calls end the
    process, even for a menu shown inside a component): `gui.menus`' and `gui.midi`'s menu pictures (`png.*`). Every
    other probe is headless (HeadlessHost, SoftRaster) and needs no display. On an X display, a window manager too:
    JUCE 8.0.4 sets `WM_PROTOCOLS` on each window with an atom it only looks up, which on a bare X server nothing has
    made, and the X error ends the process (`xvfb-run` alone is not enough; Xvfb with openbox is);
  - **a premise the machine did not give**: `plugin.reload`'s `wait.table_read_first` (and LISTEN's
    `listen.other_table_no_field`, which needs that read) wants the audio thread's blocks closer than
    `Compiler::audioIdle`'s 20 ms: a try whose blocks came 15 ms apart or more is made again, eight at most, each saying
    how far apart they came, and the rows are skipped only when every try stalled.

  What differs there and is checked as it is: `gui.shaders` builds the other renderer's profile as Metal's
  (`text.metal`: SPIR-V is the panel's own); `gui.folder` finds Show in File Manager; `gui.explorer` scrolls a row into
  view before it clicks it (CLAUDE DESKTOP shows above LIBRARY when the MCP link listens, which hangs on the build
  folder's path: a socket's must fit `sun_path`, 104 bytes on macOS and 108 on Linux). Before Linux 6.13 a file's time
  moves by the kernel's tick (1 to 10 ms), and JUCE reads a file's time on Linux in whole seconds, so `FolderSync`'s
  listing and `chatstore::Stamp` hold the file's identity (its inode) beside its size and time: a save that renames a
  new file in is a change within a tick too. `plugin.chats`' `store.conflict`, and on POSIX `plugin.folder`'s
  `atomic.rename_over` and `atomic.jetbrains`, give the new file the old one's time back (`store.theirs_alike`,
  `atomic.*_alike`: the same size and time, another identity), so they hold on the identity alone on every machine,
  APFS's and Linux 6.13's nanosecond clocks too (Windows' FolderSync keeps no identity; NTFS's clock is 100 ns). A save in place of the same size within one tick is not seen, which
  no person's saves are, and `plugin.folder`'s in-place writes move the file's time on, as theirs would be apart. `Shaders.cpp`'s `run` takes a child that exits 127 for a shaderc that did not run (POSIX lets posix_spawn
  report a failed exec so; glibc's does under Rosetta: `gui.shaders`' `cache.passing_failure_*`). Under Rosetta,
  configure with `-DFP_PROBE_TIMEOUT_SCALE=3`: the probes' timeouts are a native machine's, and lavapipe is slow.

## The installer

- **Never install from a test.** The probes (`Tools/probes/installer/`) drive the engine with `Places::under(<root>)`, a
  folder of their own under CTest's sandbox, and a `System` stand-in (`Fixture.h`'s `FakeSystem`: ditto (robocopy) for
  real, inside that root; the Trash a folder of it; the AU registrar, the running apps and the administrator prompt
  pretended, the prompt running the engine's script's text with `/bin/sh -c` (PowerShell's -EncodedCommand) as the user
  in the root). Never `MacSystem`, `WinSystem`, `Places::thisMachine` or the app in a probe, and never the installer app
  itself in a session: it writes `~/Library/Audio`, `/Applications`, `/Library` (`%LOCALAPPDATA%\Programs`, Program
  Files). On Windows the probes have bodies of their own (the same sections over its parts and folders: junctions for
  links, an icacls deny for a folder that may not be written).
- **An administrator only through the system's prompt.** "All users" is one script the engine writes (`adminScript`, every path
  through `shellQuote`; `installer.engine` checks that each path it names appears only quoted, in a root whose name has
  spaces and both quotes), run as root by osascript's `do shell script ... with administrator privileges`, its text and
  prompt passed as arguments, never spliced into AppleScript and never a file: root runs nothing this user could change
  while the prompt waits. The app never asks for, sees or keeps a password; nothing else runs as root, and the AU
  registrar is restarted by the app afterwards, never by the script. Windows: one PowerShell script (every path a
  `psQuote`d literal), its text given to System32's powershell.exe as -EncodedCommand through UAC (ShellExecuteEx
  "runas"), never a file; .NET calls alone, no cmdlet (none loads a module from a folder this user could change), and
  robocopy and icacls by their full paths; its exit code is its report (0, or 1000 + 100 * step + reason).
- **Root writes no path the user could swap.** The script's status file is in a folder it makes itself (`mkdir`, which
  fails on anything there, a link too) in `Places::scratch` (`/private/tmp`: root's, sticky), and gives back to the user
  as it ends; before a step moves anything it checks every folder on the way for a link and pins the folder (`cd -P`,
  then `pwd -P` against its real path), working by names from there; the Trash it moves into is pinned the same way;
  every rename is `mv -h` (a link where a copy goes is never followed). `installer.engine` runs the scripts against a
  planted status folder, a parent folder swapped for a link after the engine looked, and a Trash that is a link.
  Windows' script writes no status at all and works in Program Files (the administrators' alone); it checks every folder
  on the way for a reparse point (a symbolic link or a junction) and refuses a target that is one, which
  `installer.engine` tries with a parent folder and a target swapped for junctions.
- **Nothing is deleted.** A replaced copy, an uninstalled one and a half-made staging copy go to the Trash (the user's,
  through NSFileManager; the script `mv`s into it; the pages never promise Finder's Put Back, which works only for what
  Finder trashed); only an empty staging folder is removed (`rmdir`), and the script's status folder, the installer's
  own. A staging folder an earlier run left (holding only that part, or `old`) goes to the Trash before the part is
  installed again: JUCE's VST3 scan looks into hidden folders. No `rm` in the engine or its scripts (`installer.engine`
  checks the scripts). Windows: the Recycle Bin through SHFileOperation (FOF_WANTNUKEWARNING: what it cannot take is
  never deleted without Windows' own warning); the administrator's script leaves what it replaced or removed in its
  staging folder, given to the user (icacls), and the engine, as the user, moves it to the Recycle Bin after the run
  (`collectStage`); the script's one delete is `[IO.Directory]::Delete($st, $false)`, an empty folder.
- **The known folders only.** The engine writes only into the parts' folders (Engine.h's table), below `Places::home`
  and `Places::root`; every folder on the way must be a real one, so a link (a plugin folder on another disk, a linked
  bundle; a junction) is refused in words and never followed, and a scan does not look through it. A part is replaced in
  one rename (`RENAME_SWAP`; Windows: three renames, once the folder still takes a new entry, the old copy put back on
  a failure: a moment without the part, never a broken one), so a failure leaves the old copy whole; a failure stops
  the run, and the report says what was and was not changed.
- **The factory projects ride inside the bundles** (compiled into each plugin binary): the payload has no part for them.
- **No product named.** The payload names the products (`payload.json`, the build's); the app's name comes from
  `Installer.h` (generated). A part, a scope or a page added changes Engine.h's table, the Panel's layout and both
  probes.
- **One press, one action.** Return presses one button: its repeats while held (`AppPort::returnHeld`, JUCE's keys
  down), and a Return or a click in the first `Panel::kHold` seconds of a page, press nothing (a double-click on
  Continue is not Install). A run that failed or stopped is never a dead end: Done has Back. The notes (an app or a host
  open, a product in both scopes) come first on Choose and Uninstall, one line each, whole in their accessibility
  titles; the rows close up to make room. Errors are a red that is neither accent (Theme.h's rule), never ChatStyle's
  `danger`.

## The code editor

- **Three kinds of file** (`editor/Items.h`'s `FileKind`, by the name): FMinus (the program and what it includes), a
  `.gui` file and a circuit netlist (`.cir`, `isCircuitFile`). A tab's highlighter and features follow its kind
  (`CodeAssist::configure`; a tab renamed to another kind is configured again): FMinus has every language feature; a
  netlist has its own highlighter (`CircuitHighlighter`, over `lang::lexCirLine`, in the code's palette), the problems
  (RUN's and the check's: `Patch::check` generates every netlist of the files, an un-included one too, never a job) and
  nothing else (no completion, signature, hover of symbols, occurrences, Format, Cmd-/), SPICE in its status line;
  `Lang` reads a netlist line by line into the module it generates (its name, header, init, set, tick and clear, never
  step or the fields, which its configuration decides), which the FMinus tabs complete, hover and go to. Never lex a
  netlist with FMinus's lexer, nor format one.
- **The editors keep each line's colours** (FunkGui's TextEditor since v0.25.0): a highlighter runs once for each line
  an edit reaches, never per drawn row. What it reads besides the line and its state must say when it changes:
  `CodeHighlighter` colours the index's module names and constants (`TypeNames`), so `CodeAssist::sync` (called when
  the files rest, and by every language feature) rehighlights every FMinus tab when those lists change and the Panel
  draws it (`CodeAssist::highlights`; `gui.code` TYPES). The highlighters allocate nothing per line (a token buffer of
  their own, names looked up as views).
- **A netlist's `*@` lines are its schematic's drawing, invisible to the program** (`fminus/CirDoc.h`): FMinus, its
  self-test's key, ngspice and LTspice read them as comments, and every comparison of the draft with what plays goes
  through `cir::sameProgram` (a `.cir` compared without its `*@` lines): the edited light (and the IR view's), CodeAssist's
  RUN-kept messages and its check request (a change to `*@` lines alone asks for none; the same bytes again ask for
  one, so that a squiggle an edit and its undo lost comes back), plugin_state's "What plays" and get_status, listen's,
  compare's source line; messages land on a netlist's tab through `cir::lineMap` when only its `*@` lines moved;
  plugin_state sends a `.cir` without them (a line says how many) and hashes it so, a change to them alone sends nothing;
  the journal says "redrew". A new site that compares the draft with what plays uses `sameProgram`. The project's unsaved
  mark still counts them (they are part of the file), and the header readers (Lang's module doc, ChatPrompt's `headerOf`)
  skip them.
- **The schematic is a view of a file, never a tab** (`editor/SchematicView.h`): a .cir tab's Schematic view adds no
  file, so a feature that walks the tabs still walks the files; the tab's TextEditor is the model (the drawing is
  `cir::reconcile` of its text, made again only when its revision moves), every gesture is one `applyEdits` of
  `cir::apply`'s patches (one undo step either view undoes; a gesture whose base revision a merge moves is cancelled),
  and viewing never writes the file. Its layout lines are invisible to the program (`cir::sameProgram`, above): a
  gesture that only moves things leaves the program RUNNING and asks for no check. Its fields (editing a part in
  place, the Label tool's net name) are a gesture too: what Return writes is one `cir::apply` of set and rename ops,
  refused in the field when apply refuses it, and closed when a merge moves the text under it; its clipboard is
  netlist text (`cir::copied`, then apply's paste op), never anything of its own. It publishes the places it drew for
  parts and terminals that have no line of their own (its session pins: `EditorPort::setSchematicPins`, in the
  processor's memory only), which Claude's edit_circuit and view_panel's schematic picture reconcile with, and a
  file's view takes its first ones from there (an editor opened again, a picture's Panel, which publishes none), so
  every writer and viewer draws what the user sees. Each gesture gives the files to the processor at once
  (`EditorPort::setDraftIf`, through the Panel's sync hook), so Claude's conditional write sees it; when a write of
  Claude's came first that the Panel has not merged, the gesture is undone and the strip says to do it again (neither
  is overwritten). Typing reaches the processor the same way, in the key's own event (`Panel::key`: a draft not merged
  merges first, so the key lands on it, then `giveTyping`; `Panel::pushFiles`, never over a draft not merged: that
  one merges first, each tab keeping what was typed where the draft left its file, and a tab added, renamed or closed
  since staying so where the draft left that file as given: `Panel::mergeDraft`, which says so when a file both
  changed shows the draft's, the typing an undo away); a field open when the view goes is
  written, as a press outside it. The view, zoom and pan of each file are the session's alone (`EditorPort::fileViews`; never in the
  Draft, a project, an export or Claude's view), kept on the user's gestures only. Keep its colours the code editor's
  (the marks are the tab's own `DecorationInks`) and its drawing in `SchematicDraw`, which a picture draws with no
  Panel.
- **The panel under the code is the user's view, nothing more** (`Panel::setCodePanel`, the status line's Panel switch,
  Cmd-J): it hides the dials or the interface in the CODE view only, its two choices (the code's, the interface
  files') are the session's (`EditorPort::codePanel`) and written on the user's gestures alone (a picture's Panel reads
  them, never sets them), and it changes nothing that plays, no dial and no page. The PANEL view and view_panel's
  panel pictures always show the panel; the window picture shows the editor as the user has it. A hidden interface is
  `GuiView::clear`ed (its pages and scroll kept); `GuiView::place` places nothing of a spec its layout was not made for.
- **The code window holds the editor itself, never a copy** (`Panel::lendEditor`, `takeEditorBack`; `gpu/CodeWindow.h`):
  the FilesItem (its TextEditors and their undo steps, CodeAssist, the IR tab, the netlists' views) moves to the code
  window's Panel with what describes it (the draft it was given, `syncedFiles_`, `seenDraft_`, the IR viewer's count,
  the probes' owner), and every hook it holds is bound again to the Panel that holds it (`bindFiles`: a new hook into
  the Panel goes there). Both Panels are made alike, so the item keeps its index and ids. The editor's window keeps a
  hidden copy (the files as the code window's Panel loaded them) that takes and gives nothing while the editor is out:
  it merges no draft, gives the processor no files (`refresh`, `pushFiles`, the sync hook), asks for no check, and never
  has the focus or a key (no hidden item does: `setFocus`, `textItem`); the status's edited mark is the code window's.
  What the editor's window does that reaches the files goes to the code window's Panel (Run, Cmd-Return, and Cmd-S's
  and PROJECTS' save: `pushEditor`), and its Examples, Library and Random hide. view_panel's window picture shows the
  code out only while an editor has it out (`EditorPort::codeWindowOpen`), not by the session's choice alone. The window is made and let go after the event that asked (never inside the click: its own
  window may be the one that goes), takes the editor back before it goes, and when the editor goes first the code
  window goes before it, the session left out. A Panel drawn headless has no window's owner and no window button.
- **The folder is a mirror of the draft, never a second draft** (`plugin/FolderSync.h`): each pass compares each file's
  draft text, folder text and base (what both last agreed on, a hash, in the folder's `.fmsync` manifest) and moves
  only one side: the folder unchanged since, the draft's text is written there (once the draft rests, 0.3 s, at most
  1.5 s, each write a temporary file renamed over); the draft unchanged since, the folder's lands in it through
  `Processor::folderDraftIf`, version-guarded (only while the draft is what the pass read: a write of Claude's or
  typing that came between is never overwritten; refused, the next pass decides again), every file of one save in one
  write so an open editor merges it as one undo step; both changed, a conflict: the file held, neither side written,
  until `resolveFolderConflict` keeps one and puts the other in `.conflicts/`. Nothing is ever lost: no path writes a
  side it has not compared, the folder's changes are taken only once its listing held still 0.15 s (an editor's save
  in steps; a file's size, time and identity, its inode, so that a save renaming a new file in is seen within one tick
  of the file clock too, which on Linux before 6.13 moves by the kernel's tick), and a write of the plugin's goes only
  right after a look that found the file as last seen. The texts are
  compared as the editor holds them (`TextEditor::sanitise`). The program (the first file) is never deleted from the
  folder's side; a name `validFileName` refuses, a 17th file, an editor's swap file stay there (a note); host.fm and a
  library file an `#include` names that the draft lacks are read-only copies, never taken (host.fm in the draft would
  shadow the plugin's); a copy as written is a base, so a draft file of its name (a project's own) is the plugin's
  change over it, and a draft file that becomes a copy is written over quietly when the folder's is as last agreed.
  One instance a folder (`.fmsync-lock`, flock); the folder is the instance's, so a project opened or a state loaded
  flows out as any edit (a save not yet taken is then a conflict). The audio thread never sees any of it; the editor
  reads it through `EditorPort::folder` and `folderRevision`. Probes never launch an app: `openFolder` goes to
  `Processor::setFolderLauncher`'s hook, and the default refuses under a probe's sandbox.
- **A dropdown is one trigger and FunkGui's menu** (`editor/Dropdown.h`): every control that opens a menu of choices
  (Examples, Library, the project's pill, the zoom, the chat's model and thinking pills, the Settings card's selects, a
  `.gui` file's `menu`, the Schematic view's More and probe chip) draws through `dropdown::drawTrigger`: a pill in one
  of three bases, the chevron at `kChevronInset` from its right edge, hover, open (the accent's edge and tint, the
  chevron up) and the focus ring `kRingGap` outside it; a click, Return and Space open its menu, and so do a screen
  reader's press and showMenu (a `.gui` menu through `GuiView::Hooks::menuHost`; the GUI sandbox has no host, so there
  they step its value); a new one does the same and is a combo box to a screen reader. The schematic's two are combo
  boxes to a screen reader (`SchematicView::accessibility`, with its toolbar's other buttons, the circuit dials, the
  info strip and the probe card's controls: More's choice from a screen reader is placed at once) but never Tab stops,
  as nothing in its toolbar or probe card is (the Netlist view is its keyboard's way; `A` opens More). A `.gui` menu
  is `kGuiMenuRoom` wider than its widest value (GuiLayout.h), so that value shows whole.
  Open is the Panel's `menuOpenAt(anchor)`: HostProxy keeps the shown request's anchor until its answer, `dismissMenus`
  or the next request (a serial each), so a trigger asks with the rect it anchors its menu to (`ButtonItem::setOpen`,
  `SettingsCard::setMenuOpen`, `GuiView::Hooks::menuOpen`, `SchematicView::setMenuOpen` through `FilesItem`; none in the
  GUI sandbox), and its drawKey mixes it. The rows are FunkGui's (0.25.0): a set of choices is `dropdown::choice`
  (radio, the current one checked) with `dropdownMenu` (anchor, `minWidth` the trigger's, `selectedId` the current one);
  a long list's sections are `header` rows, never disabled items; a command's key is `command`'s shortcut column, never
  part of its label (the probes choose items by label). Every host shows every field of a request: FunkGui's
  (HostServicesJuce, HeadlessHost) and `nogpu/SoftEditorHost`, which keeps HostServicesJuce's way (`validMenu`,
  `MenuLook::popupMenuFor`, its Options); the probes run on HeadlessHost only, so a change to one is made to the other
  by hand. The menus are native windows in MenuLook's style, which keeps no search field, so a label starts with what a
  user would type (the type-ahead). No canvas popover: a native menu may reach past a small plugin window, scrolls,
  takes the keys and speaks to a screen reader on every platform. `gui.menus` checks.
- **`fminus/Lang.h` is the language, as an editor sees it**: plain C++ over the files' text, no FMinus call. Its
  tables (keywords, type words, the builtins with their signatures, the attributes, `#pragma panel`'s words) follow
  FMinus at its pin and PanelSpec: a pin move that adds a keyword or a builtin adds it there, with `fminus.lang` rows.
  A string literal is a token of its own (`Tok::string`: an attribute's argument, the one place FMinus reads one), and a
  declarator's tail is read as FMinus reads it: its attributes, then at most two sizes (`declaratorTail`), a field's
  and a param's attributes kept (`Symbol::attributes`, an unknown name without a namespace dropped as FMinus drops it).
  Its index has what C declares: an enumeration's tag and constants (the top level's, a module's, a block's in its
  scope; each constant's value folded as FMinus folds an integer constant expression, `Folder`, across the files, a
  name as the constant its place sees, as C's scopes choose it, `constantAt`; C23's fixed type, `: int` or `: bool`,
  and its tag declared ahead of its list, a tag with no body; the names after a list as a declaration of its type
  there declares them) and the top level's const tables and scalars, which complete, hover, go to and highlight
  (`constantNames`, the editor's `csConstant`) as the other symbols do; a circuit field's extent of enumeration
  constants folds so too. A pin move that changes what a declaration may be changes `Indexer` and the formatter with
  it (`fminus.lang` rows over a program FMinus compiles). `Folder` recurses at most
  `kMaxDepth` levels (63 parentheses): Patch folds every program's constants on the compiler's thread, whose stack is
  a thread's default. `foldValues` folds in the order the compile reads the files (`ReadOrder`: the first file from
  its top, an `#include`'s file where it stands, once; the files no `#include` reaches after, each alone), so its first
  pass folds all FMinus folds, and makes at most `kFoldPasses` (what FMinus refuses but Lang reads on, a name of a list
  after it, costs a pass a link: never a pass for each constant). Another module's enumerations and their constants
  are the file's from their place on in that order, as FMinus has them (`constantAt`, `globalNamed`'s `at`, completion,
  a circuit field's extent): a use before the module is FMinus's error and Lang's nothing. A constant's name that a
  local, an argument, a field or a param has too (`shadowNames`) is coloured as what it is at each use: `shadowed`
  gives each use's flag (`resolve`'s answer), which CodeAssist's sync puts in `TypeNames` by tab and the highlighter
  counts its way to in its state (bits 17 to 31, the conditionals' count in bits 1 to 16); a tab without flags, or a
  use past them, stays a constant, and while the index is due after an edit (until the check's sync) a use after one
  typed or deleted may show its neighbour's flag. The flags are the tab's by its name: a highlighter is made for a
  name (`CodeHighlighter::file`), and sync gives a tab renamed one of its new name. A hover card's code is drawn with
  `Hover::plain`, the names its declaration declares, never as constants. A hover's code is fitted to the card
  (`fitCode`: `kHoverWidth` columns, `kHoverLines` lines, broken after a comma in brackets or before an attribute,
  outermost first), and the card cuts a code line still too wide with "..." (never a piece of it left out further
  on). A cut of text that may hold a string's characters keeps whole UTF-8 characters (`lang::headOf`).
- **The plugin's macros are every build's** (`lang::kDefines`: DEBUG; Patch.h, "Conditionals and warnings"): each of
  Patch's compiles defines them (`Program::Options::defines`), the check's and the frozen copy's too, which plays
  through the program's instances and so must be the program (a field an `#ifdef DEBUG` declares is in both). A new
  macro is a `kDefines` row (its hover's doc with it) and a guide line, and a name a user's program may already use in
  code is a broken program: name one as C names a host's. What reads a program's text itself follows the groups the
  compile reads (`lang::conditionals`, on the files in the compile's order): the index (a skipped group has no
  symbol), the highlighter (`CodeHighlighter` dims a skipped group: its state counts the file's conditional
  directives, which index `ui::Conditionals`, a snapshot CodeAssist makes again when the files' change, setting the
  highlighter of each tab whose directives read otherwise), the formatter (a skipped group as it is) and the panel's
  `#pragma` lines (Patch reads them from the files as FMinus read them). FMinus's warnings (-Wswitch) are a compile's
  notes and the check's messages, `warning:` lines the editor shows in amber; run_program says them on one line.
- **The formatter never changes what a program means**: it re-indents, re-spaces and breaks lines (before a `{` that
  opens a block, around a `}` after code, and an initialiser's list longer than `lang::kWidth`, 120 columns, after its
  commas), never joins them, and it compares the tokens it wrote with the text's and
  gives the text back unchanged when one differs. `fminus.lang` renders the examples and RANDOM programs formatted:
  the same bits.
- **The check while typing** runs on the compiler's thread after a RUN waiting, never swaps the program that plays,
  and its messages land only in tabs whose text is what was checked (a later edit waits for the next check); RUN's do
  the same. RUN's messages are kept (`CodeAssist::setRunDiagnostics`): a check of the files RUN compiled shows them
  again, as they hold what only RUN finds, and RUN cancels a check due. The chat's message box is a TextEditor too and
  gets none of this.
- **The IR tab is never part of the program** (`editor/IrView.h`; `Compiler.h`, "The IR"; `gui.ir`). It is a view of
  what FMinus made of the program (LLVM IR: optimised, lowered, the frozen copy's), not a file: never in `Draft` (so
  never compiled, read by an `#include`, saved in the session or a project, exported, or sent to Claude), counted by no
  rule of the files' tabs (`kMaxFiles`, the names, the renames), its editors outside `FilesItem::tabs_` and outside
  CodeAssist's loops over the files (`Tabs::active()` is `count()` while it shows: no language feature, no
  diagnostic, no check), read only (`TextEditorOptions::readOnly`, and the edit keys taken with a notice, never the
  host's). Keep it so: a feature that walks the tabs walks the files. Its cost stays with whoever looks: the processor
  counts the views open (`EditorPort::addIrViewer`, a Panel's while its tab is open, and when it goes), and only then
  does a compile keep the IR (`Patch::compile`'s `keepIr`; the freezer's copy, `Patch::freeze`'s `ir`) or the
  compiler's thread make it for the program that plays (`Patch::ir`, `frozenIr`: the options the code came from, no
  code, nothing offered); `compileNow` never prints it (it may run on the message thread), the audio thread never
  sees it, and the texts are cut past `Compiler::kIrMaxBytes` before they reach the editor, which takes them shared
  (`IrText`); with nobody looking, nothing is printed and what the compiler held is let go (a program compiled while
  someone looked keeps FMinus's own copy in its `fminus::Program` until it is replaced). A view's text is made again
  only when what it reads changes (`Panel::syncIr`: its comment lines, `irview::header`, and its `IrText`), not with
  each move of the IR's revision (the frozen copy's moves it too, which the program's views do not show), and its
  selection and scroll follow LLVM's text. The IR tab takes no more of the bar than the files' tabs leave at their
  narrowest, so none runs under it. A new view is a row in the explorer's LLVM IR section, a `View`, its text in
  `irview::header` and `irview::shown`, its texts in `IrStatus`, and `gui.ir` rows.

## The chat (Claude)

- **The network.** The chat is the only code that reaches the network at run time, through `claude::Transport`
  (`ClaudeApi.cpp`: HTTPS, or HTTP to a server on this Mac or the local network; never following a redirect,
  `withNumRedirectsToFollow(0)`: a 3xx comes back with its location, as an error; JUCE's client takes the address
  as built, `createWithoutParsing`: a parsed query goes into a POST's body), to one host at a time, and only when
  the user sends a message or chooses a provider: `api.anthropic.com` with the user's own key (the Messages API,
  streamed; `GET /v1/models` once per key), or the provider the user chose (below, "Other model providers"): its own
  server alone, with its own key. The build never reaches the network. Probes never do (`plugin.providers` REDIRECT
  runs the real transport against two servers of its own on 127.0.0.1, nothing beyond): they give the agent
  `fake::Transport` (`Tools/probes/plugin/FakeClaude.h`, a scripted API, and the other providers' recorded answers:
  `OpenAiSse`, `GeminiSse`, their models lists) and a key folder of their own (`chatstore::setDirectoryForTests`;
  under CTest the default folder is the test's sandbox, `<prefix>PREFS_DIR/chat`). A session never makes a live call to
  Anthropic or another provider (it is billed to the user) unless the user asks for one.
- **The key** lives only in its encrypted file (`ChatStore.h` says the format and what it protects against):
  `anthropic-key.bin`, in the folder both products share. Never log it, print it, put it in the session, a probe's
  output, a test fixture or a commit; the panel masks it (`Panel::maskedKey`: the preset's public prefix at its own
  server, stars, and the last four of a key of 20 characters or more) and speaks only its length. A probe's is
  `fake::fakeKey()`.
  With no key stored the chat asks for one, and for nothing else. Another provider's key is sealed the same way in
  `key-<kind>-<16 hex of its server's origin>.bin`, the origin inside it (`chatstore::saveProviderKey`): it is read back
  for that server alone, so a key never goes to another server; an Anthropic key is refused for another provider; the
  same rules hold for it (never logged, printed or kept anywhere else; a probe's are `fake::fakeOpenRouterKey()` and
  its siblings).
- **The chats: on this Mac only** (`ChatStore.h`'s chats, `ChatAgent.h`'s "The chats", `plugin/ChatChats.cpp`). A
  conversation is kept with its project, or, while none is open, with its instance, in the store's folder alone,
  sealed with the key's machine-bound keys (FPC1), and nowhere else. The rules:
  - **How a project is known.** By its name, which is its file's (`ProjectStore.h`: a rename in the Finder is a
    rename), and since the chats by the UUID in its file (`"id"`), made at its first write (or when `idOf` is first
    asked, for a project from before ids) and kept by every later write, so a rename keeps it. `idOf` gives a Finder
    copy (another file with the same id whose name is not the `"name"` written in it while the original's is, else the
    one made later) an id of its own; `add` (an import) always makes a new one; an export (`ProjectExport`) clears it.
    The session keeps the project's id beside its name (`<PROGRAM projectId>`), so a project renamed since is found
    by it (`nameOfId`). A factory copy of a user's project must leave the id out too.
  - **Who owns the chat shown.** The processor's `chatOwnerNow`/`syncChatOwner`: the project open's id, else
    `instance-<id>` (`instanceId_`, kept in the session's `<CHAT instance>`). Opening a version of another project
    (`openVersion`: PROJECTS, a preset of another project, A/B's Keep), a fresh start (EXAMPLES, RANDOM) and deleting
    the open project are `open` (the chat shown is kept with its owner; the new owner's newest shows, else a new one);
    a save is `carry` (from the instance the chat moves, from another project a copy goes), or `move` when the
    project the chat was with has no file any more (deleted, or a new file made in its name's place: the save's id,
    `Saved::id`, tells, and `chatOwnerNow` looks the project up again). A project the plugin's own chat opens in its
    turn (the projects tool's open: `openProjectAs` with `Journal::Who::chat`) is a `carry` too: the conversation that
    asked for it goes on with it, never left behind with the project before. An `open` asked from a turn's own thread
    (Claude Desktop's projects open, which runs as a turn: the user's own chat changes project, as with PROJECTS)
    waits for the turn's end (`pendingOwner_`), never joins the worker from itself; a `carry` runs at once. A switch
    stops the turn and joins it on the message thread, so nothing a turn does waits for the message thread past
    `ChatAgent::stopping()` (`chatPicture`, view_panel's wait for `poll()`; `onMessageThread`, the projects tool's).
    Deleting a project deletes its chats (the Projects card says so) unless a Finder copy holds its id.
  - **One instance, one id.** No two live processors of a process hold one instance id (`heldIds`), and a state's
    `<CHAT instance>` is taken only when it is this instance's own, or when its chats are there to find (`hasChats`)
    and no other live processor holds it: a state copied into several instances (a track duplicated, a template, a
    default preset) gives each its own chats while no project is open. A copy of a live instance's state shows the
    chat it names as a new chat of its own (`restoreChat`'s `from`); a state whose instance kept nothing here (a
    template from an instance that never chatted, another Mac's) gets a new id. An instance's chat is the one its
    session names, else a new one, never another of its chats (a project's is its newest).
  - **When it is written.** `ChatAgent::keep` (`saveState(true)`: the panel's pictures kept, so a chat opened again
    keeps its thinking) at a turn's end, at `getStateInformation` (`keepIdle`: not while a turn runs, as a host may
    ask at every change), before another chat shows, on a new chat and when the agent goes; only when it changed and
    holds more than the chat's notes; one writer at a time (`storeMutex_`, never under `mutex_`; a switch stops and
    joins the worker before it takes it). A chat file another writer changed since it was read here (two instances
    with one chat; `chatstore::Stamp`, its time, size and identity: each save renames a new file in) is a conflict:
    this one is kept under a new id. A project's chat whose folder is gone since it was
    kept here (the project deleted in another instance) is not written back. A write that fails (or is held back so)
    says so in the chat once, is tried again at every keep, and says so again once it is kept: the session no longer
    holds the chat.
  - **The session** keeps `<CHAT instance owner chat>` and never a word of the conversation. A session from before
    (`<CHAT json>`) is adopted when it holds more than the chat's notes (`ChatAgent::adoptable`; every untouched
    instance's empty conversation was alike, and so was a lone note): its id `sessionChatId(json)`, and the instance's
    too when the chat is the instance's, so loading it again finds the same chat; else a new instance id and a new
    chat. The next save leaves it out. A reference that resolves to nothing here (another Mac) shows the project's
    newest chat, else a new one.
  - **What can carry a chat off this Mac, and what holds it back** (a change to any of these keeps the rule):
    1. the DAW session, a host's presets (.aupreset, .vstpreset), its undo history, a copy and paste of the plugin, the
       Standalone's settings file (`filterState`): all `getStateInformation`, which holds references only (an older
       session still holds its conversation until it is saved again; the plugin cannot rewrite a host's file);
    2. a project's file, `.fmpack` and `.zip` exports and their README, a factory project: never hold a chat; the id
       is left out of exports (`plugin.chats` EXPORT);
    3. Claude Desktop (MCP): no tool, `list_instances`' info, the guide, `tools.json` or `guide.md` holds a chat's
       words (`get_status` checked); what Claude Desktop does shows in the chat, not the other way;
    4. the journal (`get_status` since) records who changed what, never what was said; captures hold audio and MIDI
       in memory only; the folder sync mirrors the program's files only;
    5. backups, Time Machine, a copied Library, Migration Assistant, a stray search: the files are sealed with keys
       derived from this Mac's device ID and the user's account, useless elsewhere (`ChatStore.h`);
    6. the clipboard (a Copy the user clicks) and the conversation sent to the API with each request (the chat's
       purpose: `api.anthropic.com`, web search's fetches when the user turned it on, or the provider the user chose,
       its server alone: HTTPS, or HTTP on this Mac or the local network only) are the user's own acts; `chat.json`
       holds the provider, its server's address and the models chosen, never a word of a chat;
    7. logs: nothing of the chat is logged (the MCP server's stderr says protocol events only).
  - **The list** (`ChatPort::chatList`, the chat card's pill, `Panel::openChats`): the owner's chats, the newest first,
    the one shown dotted, each one's time in the shortcut column; New chat, Rename (the key dialog's box,
    `renaming_`) and Delete (asked first). `plugin.chats`, `gui.chat` CHATS and `gui.menus` CHATS check it all.
<!-- origin -->
- **No accounts.** Until 2026-10-28 the chat could also sign in to a TestingPlayground server and go through it
  instead of a key. That is gone (the repository is open source): the agent deletes what it kept when it starts
  (`chatstore::forgetOldSignIn`: `account-token.bin` and `chat.json`'s "account" and "use"), and a session from then
  loads as any other. `plugin.chatstore`'s OLD and `plugin.chat`'s OLD check it.
<!-- /origin -->
- **The API** (Anthropic's). Requests follow the Claude API reference (the `claude-api` skill): the `anthropic-version` header
  (`claude::kApiVersion`), adaptive thinking with `output_config.effort`, no sampling parameters, the system prompt
  cached for an hour and the automatic breakpoint, tools with `eager_input_streaming` whose input is parsed strictly
  here (`INVALID_JSON` results), `"fallbacks": "default"` with its beta on the models that take it, fast mode
  (`"speed": "fast"`, chosen with the Fast pill, on Claude Opus 5.5 without a beta and on Opus 5 and 4.8 with
  `fast-mode-2026-02-01`; such a request goes without `fallbacks`, a 429 or 529 twice runs the turn on at standard
  speed, a refusal about the speed turns it off), and an append-only history (preserved thinking: nothing sent before
  is changed, except as the fallback rule says). Check the reference before changing a request; `plugin.chat` checks
  the shape.
- **Other model providers** (`plugin/Providers.h`, `ChatProviderTurn.cpp`; `ChatPort::setProvider`, the key dialog's
  Provider pill; README, "Other model providers"): an OpenAI-compatible server by its base URL (the presets OpenAI,
  OpenRouter, Ollama and LM Studio on this Mac, or any other) and Google Gemini, beside Anthropic, the default. The
  rules:
  - **Anthropic's path does not change.** No provider chosen (chat.json without "provider") is Anthropic; its requests,
    its key, its models and its options are the members and the code they were (`model_`, `models_`, `key_`;
    another provider's are `provider_`, `otherModel_`, `otherModels_`, `otherKey_`), and its probes (`plugin.chat`,
    `gui.chat`) are unchanged. The one difference in its requests: `providers::forAnthropic` takes another provider's
    own fields off the history (a Gemini call's `thought_signature`), and gives the history as it is when there are
    none: the builder's request (`buildRequest`) and the planner's (`forAnotherModel` starts from it) both.
  - **One history, one loop.** The history stays in the Messages API's shape whatever the provider; another provider's
    request is `buildRequest`'s translated (`providers::body`: its model, system, tools and messages; everything else
    is Anthropic's alone and left out) and its streamed answer translated back into the Messages API's events
    (`providers::Stream`, into the same `MessageBuilder`), so the tools, the checks and their nudges, ask_user, a reply
    cut off, the retries and the chats work unchanged. A call's id is made here (`call_` and 24 hex digits), so a
    chat goes on with another provider. Another model's reasoning shows as thinking and is never kept (a thinking block
    without Anthropic's signature is refused there).
  - **What is Anthropic's alone** is off for another provider, never broken: fast mode, web search, the planner
    (`ask_planner` refuses), the cache marks, compaction, fallbacks; their pills are hidden and their setters refuse.
    `limitNote` is Anthropic's.
  - **Thinking levels** (`providers::Reasoning`, `levelsOf`; `ChatModel::efforts`, `defaultEffort`): the same Thinking
    pill and `setEffort` drive another provider's own setting where its model takes one, and only when the user chose
    a level (none chosen: nothing sent, the model's own default, shown as the level when its server says it; the menu's
    "The model's default" row clears a choice where it does not). OpenRouter (host openrouter.ai): `"reasoning":
    {"effort"}` alone (never `max_tokens`), the levels from its list's `reasoning` object (`supported_efforts` put in
    the menu's order, Off dropped when `mandatory`, `default_effort`, Off by default when `default_enabled` is false;
    no object: no pill); its `reasoning_details` text shows as thinking when no `reasoning` text came, so once. OpenAI
    (host api.openai.com): `reasoning_effort`, the levels by the model's family (`openAiLevels`: o1/o3/o4, gpt-5
    with minimal, gpt-5.1 with none as its default, gpt-5.2 with xhigh, later ones low to high; none for `-chat`,
    gpt-4 and before). Gemini: `generationConfig.thinkingConfig`, `thinkingLevel` from Gemini 3 (pro low and high,
    flash minimal to high) or `thinkingBudget` on 2.5 (1024, 8192, 24576; 0 for Off on flash and flash-lite only),
    never both, `includeThoughts` unless Off; its list's `"thinking": false` takes the levels away. Any other server:
    none. Kept per provider and model in chat.json's `"efforts"` (`ChatAgent::otherEfforts_`,
    `chatstore::ProviderChoice::efforts`). A 400 that refuses the level (`aboutEffort`: the parameter named,
    OpenAI's words for one the model does not take, or Anthropic's through OpenRouter for thinking on with no
    thinking block in the history, a tool call's next request, as the reasoning is never kept; OpenRouter's
    "Provider returned error" read with the provider's own words from its `metadata.raw`, `errorOf`) is
    `workAround`'s first case: asked again at once without it, the model's levels gone for the agent's life
    (`effortRefused_`, applied to every list read after) and its kept level forgotten, a note; never a failed turn.
    The provider's note says the Thinking menu stays where the model takes a level (its own levels, its default
    until one is chosen: no level of Anthropic's carried over). What the docs say and no probe could check live is
    in the README.
  - **Degrading**: a 400 that says the model takes no pictures (`aboutImages`, while the request holds one) makes the
    request again with a line of text in each picture's place and marks the model so (`workAround`, a note); a model
    its list already marks so gets the same lines, said once a chat (`picturesNoted_`); a 4xx that says the model
    cannot serve this chat (`aboutModel`: OpenAI's Responses API alone, not a chat model, no tools) takes it off the
    menu and out of the choice, and the error says to choose another (never a model switched to here); OpenAI's list
    leaves those out from the start (its `-pro`, codex, deep-research, `chatgpt-` and `-chat-latest` models); Gemini's
    400 refusing `parametersJsonSchema` makes it again with Gemini's own schema subset (an enum of words with no type
    a STRING, as the projects tool's action and run_function's `on`; an array whose items the schema leaves open, as
    run_function's `args` and write_file's `files`, an array of STRINGs, each a JSON value written out, which
    `providers::fromSubset` reads back by the tool's own schema before the tool runs: a tool gaining such an array
    needs nothing more; a property it still cannot say, an object with no properties such as trace's `export`, is
    left out), and keeps that (`schemaRefused_`);
    a model whose context is under `kShortPromptBelow` tokens (as its list says), or one a server on this Mac or the
    local network does not describe, gets `shortPrompt()` (the prompt up to THREADS and a line of what it left out);
    a server here that read under half of what was sent cut the conversation (Ollama does, silently): noted once a
    chat. A change to the prompt's headings keeps `shortPrompt` cut at THREADS (`plugin.providers` checks it).
  - **Servers and keys**: a base URL is `https://`, or `http://` to this Mac or the local network only
    (`providers::canonical`); a key is bound to its server's origin (above, "The key") and the headers carry it to that
    server alone (`Authorization: Bearer` for an OpenAI-compatible one, `x-goog-api-key` for Gemini), and no redirect
    takes either elsewhere (above, "The network"; `redirectText`); a server here needs none. A turn uses the key of
    the turn's own provider even when the user switched meanwhile.
  - **A turn's start** (`startTurn`): the provider, its key, its model and the turn's options from one moment (a
    provider chosen while the models list was on its way, nothing sent yet, is the turn's: it looks again). With no
    model chosen, Gemini's first and a server's here are taken (`picksFirst`); OpenAI's, OpenRouter's and another
    remote server's lists are long and of every price, so the turn asks the user to choose one and sends nothing. A
    list that could not be read says why (`otherListError_`: is the server running?, the key refused, a redirect);
    its fetch from `setProvider` ends on `listCancel_` (a Stop, the agent's end), never on a turn's stop left over.
  - **Tests** use recorded shapes of each provider's documented API (`FakeClaude.h`); what no probe could check
    against a live service is listed in the README for the user to try. `plugin.providers`, `gui.providers` and
    `gui.menus` PROVIDER check it all.
- **Threads.** ChatPort calls run on the message thread; a turn runs on the agent's worker, which reaches the plugin
  only through `ChatHost` (the draft under `programMutex_`, RUN through the Compiler, a dial queued for `poll()`). The
  audio thread never sees the chat.
- **Large work.** `max_tokens` is the model's own limit (from /v1/models, at most 128K); server-side compaction (beta
  `compact-2026-01-12`, trigger 150K input tokens, instructions that keep the design and leave out code) on the
  models that take it, the compaction block kept and what came before it dropped: the history then starts at the
  block, in its assistant message (never at the message before it, which mid-turn is tool results the API refuses
  without their calls; `repairHistory`, also run on a loaded session), and the files go again with whatever follows,
  a message or the turn's next tool results. A compaction block counts only with its summary (`summarized`: its
  content a string with something in it): one whose summary never came (a Stop or a broken stream before its one
  delta, a compaction that made none, content null) cuts nothing and is never kept (`repairHistory` takes it out of a
  loaded chat, and a line stands for a start already lost to it). Blank text never goes into the history (the API
  refuses it), beside a tool call too; a reply that ends with nothing else to keep (no block, thinking alone) gets
  "(no answer)" after what it has, shown as the answer (`mendReply`; `repairHistory` mends a loaded one), except one
  cut off at `max_tokens` that thinks, below; a `pause_turn` reply with nothing in it is not kept (the request goes
  again as it was). A file's text goes in the plugin's state only when Claude has not seen
  it (`seen_`); a broken stream is retried with what it showed taken back; 80 requests a turn. A reply cut off at
  `max_tokens` (thinking counts) goes back as it came and the turn goes on with a line that tells Claude; a second
  in a row ends the turn. The prompt says what a reply holds; at xhigh and max a note with the user's message says it
  again (`limitNote`, kept in the history as sent). `plugin.chat` checks each.
- **The system prompt and the tools** bind the thinking blocks of a conversation (preserved thinking): the chat's file
  saves their hash (`promptPrint`), and a chat saved with another loses its thinking when it loads
  (`dropThinking`). Changing either is fine; editing a message already sent is not.
- **measure_output** renders the editor's files offline (`dsp/Measure.h`, a fresh compile in an Engine of its own,
  never the one that plays) and reports in words; `plugin.chatexample` checks its numbers on programs whose answer is
  known. The same pass records named numbers (`measure::metrics()`: each a name, a unit, a line of exactly what it
  measures, its span, window, weighting and band, in the words `metricTerms()` defines, and a default tolerance; the
  system prompt's CHECKING YOUR WORK lists them from the table, the terms first, each line's tolerance written by
  `toleranceText` from the same table `Goals` reads (`plugin.chatexample` holds the guide's to `toleranceOf`), so a
  metric added there is taught at once); a request's `want` runs only the renders and the analysis they need. The
  report ends with the values measured, name=value in the table's order (`metricsLine`: the guide says so), and
  measure_output's `spectrum` (octave, third or semitone: `Request::spectrum`, analysis only, as `want`) adds band levels
  as data (`bandsOf`, `bandLevels` over `bandSpectrum`: both channels' power, in `kBandSeconds` Hann frames (a
  semitone's `kNoteBandSeconds`; a shorter part's frame the part), a band's bins by their share of it; an instrument's
  in dBFS while held, an effect's noise's gain, out against in; the line names its frames and the band below which a
  tone spreads, `sharpFrom`: under `kSharpBins` of the frame's bins wide a band no longer holds a tone's main lobe), the
  octave bands compare and the capture's report read too (`stereoSpectrum` in `kBandSeconds` frames), so that none of
  them moves with the rate. Every
  metric is over its own span and the same at every rate: the held part (`heldSpans`: each note from 50 ms after its
  note-on, half the note when shorter, to its note-off; an effect's signal from 50 ms in to its end), its spectra
  Welch's average over all of it in frames fixed in seconds (`measure::spectrum`: 0.1 s Hann for the centroid and the
  partials, 0.4 s Hann for the pitch, 0.4 s Blackman-Harris for inharmonic_db and thd_db; a part shorter than a frame
  one frame of it), the centroids power-weighted over 20 Hz to 20 kHz, the pitch the strongest partial within 600
  cents of the expected note (`pitchOf`, a harmonic sum without one), read stretch by stretch (`measure::stretches`:
  the held part cut where the lowest key held or the pitch wheel changes, by `kSameBendCents` or more; a window's part
  no key is held over the key sounding there, else the key released last; each against its own expected pitch, so a
  phrase's notes and a bend through a note are followed; `pitch_cents` the farthest of them, signed, `fundamental_hz`
  and off_order_db's default base the lowest key's longest stretch's; a stretch whose frame does not part its pitch
  from 0 Hz and from the key above it, 4 / min(f, gap) seconds, is left out, and with none the pitch is unmeasured,
  naming the note it takes, or the wheel when it moves too often), inharmonic_db the strongest peak (a local maximum:
  a level that moves in a frame, a slow attack, widens a harmonic's lobe into a skirt with none) off each stretch's
  harmonics (unmeasured when its frames' zones leave no room between them: 12 / f0 seconds), one click rule (`measure::clicks`) for the click metric, the step lines and
  trace, the loudness BS.1770's (`loudness`: K-weighting made for the rate from the analog shelf and high-pass its 48 kHz
  coefficients come from, 0.4 s blocks every 0.1 s inside each span, the -70 LUFS and -10 LU gates) and IEC 61672's A
  (`aWeightedPower`, of a stereo spectrum in frames as long as the part, up to 2 s), off_order_db the power farther than
  `kOrderBins` of Blackman-Harris frames as long as the part (up to `kOrderSeconds`) from every multiple of a fixed base
  (`orders`: `Request::baseHz`, an instrument's base_hz, else the expected pitch; an effect's 220 Hz; analysis only, but
  `sameRender` tells bases apart), wobble_db each third's share of the power over the spectrum's frames (`wobble`; an
  effect's of a steady tone's output, `steadyTone`: partials every 10 Hz repeating every 0.1 s, its frames end to end on
  that period (`onPeriod`), so that an effect that holds still reads 0 however long its memory, where a ratio of noise
  out over in would read a delay's memory as motion; the tone renders for wobble_db and level_wobble_db alone,
  uncounted in step, alerts and cpu_percent), level_wobble_db the SD of the same frames' levels about their trend
  (`levelWobble`: each frame's Hann-weighted mean square in dB, floored 60 dB under the loudest, against a least-squares
  line through its span's frames within `kLevelTrendSeconds`, so a fade or a decay reads about 0 and a tremolo high;
  unmeasured where wobble_db is, with its reason) (`dsp.metrics` checks each at 48 and 192 kHz). The render settings' `window` ({from, to}, `Request::window`, at least
  `kMinWindowSeconds`) takes the held part's place: seconds of an instrument's render, which then lasts to its end,
  for the metrics "while held", peak_db and cpu_percent (the calls that render it); attack_s and release_s keep the
  notes', and the whole render's metrics the length it has without the window (`Timeline::plain`, where a call ends);
  seconds of the effect's sine's, noise's and steady tone's renders, cut at hold_seconds. One that holds too little of what a metric
  needs (as a held part under wobble_db's three frames, or a base under twice off_order_db's zone) leaves it out of the values with why (`Result::unmeasured`, a check's NOT MEASURED line; tune_dials leaves
  such a goal out of its search, and with none left does not search); `sameRender` tells checks apart by it, and the
  session keeps a check's; trace and compare refuse one. Change a definition and its line together. The report's pitch line is a judgement
  on top, the metrics untouched: with the whole report asked and the part not silent (a stretch read or none), the
  lowest note and the note a whole tone up (not an octave: a fixed pitch's second harmonic sits there), each alone
  (0.6 s, none of the scenario's moves, a single note too), must measure within 50 cents of a whole tone apart, else
  the line says "Unpitched" with both pitches and the Inharmonic line is left out. Its CPU figures are the rendering thread's own time
  (`CLOCK_THREAD_CPUTIME_ID`, read at a render's few boundaries, never a block's), in an Engine as the audio thread
  runs it with no editor open: an instrument's `cpu_percent` over the held part (or the window), and
  `cpu16_percent` measured with sixteen notes held (the least of two short renders), not extrapolated (in the
  report's words only when more than one voice sounds there: `Rendered::mostVoices`); of the
  program's own code with its dials held still, every call flat (a dial that moves costs more: the Filter example
  about 2.7x while its cutoff glides; the frozen copy that plays once they have held still a while usually costs
  less), which the metric's line in the guide says (the result's CPU line gives the numbers, and says when a scenario
  moved a dial). A `Session` compiles once and renders as often as asked, each render after
  `Engine::restart` (offline only: prepare() and `Patch::reset`), so it gives what a fresh compile gives; `dsp.tune`
  checks that, and the numbers.
- **The profile** (`dsp/Profile.h`, Measure.h's `Request::profile`, `Patch::codeRanges` in `fminus/PatchCode.cpp`):
  measure_output's `profile: true` (`scenario::profileOf`, which the renderer reads too) adds a line after the CPU's:
  the share of the thread's own time each of the program's functions took over the calls cpu_percent times (an
  instrument's notes held, or its window; an effect's counted signals), the most first, each with what the optimiser
  inlined into it (FMinus's `CodeRange::inlined`: a function inlined everywhere has no share of its own), the share
  outside the program's code (on macOS split by `dladdr` into the plugin's own code and the system's libraries, named),
  and the modules' when more than one has a share; `Result::profile` has the numbers. Sampled, not instrumented: a
  thread of the `Sampler`'s own suspends the thread that renders about every 0.1 ms, reads its registers and its CPU
  time (Mach's calls on macOS: no signal handler, nothing process-wide; `SuspendThread` on Windows) and resumes it,
  each sample weighed by the thread's CPU time since the one before (the first since `arm()`, which reads its own), so
  a thread put aside weighs nothing; between the suspend and the resume it allocates and locks nothing (the thread it
  stopped may hold malloc's lock). A sample in what the program called out of its code (FMinus's runtime: an FFT, a
  called math function, the pool; the system's libraries) is charged to the program's function that called it, on
  macOS by its return address and up to `kWalked` frame records up the stack (each checked to lie between its stack
  pointer and its stack's top, aligned, before it is read; `profile::chargesCalls()`), said apart (`Share::calls`: "in
  its calls out of the program's code"); Windows walks no stack, so that time is outside. Only this thread is sampled:
  a parallel region's work on the pool's other threads is not (`CodeRange::parallel`: the line says so when the
  program has a region). Off the audio thread only: a Sampler samples the thread that made it, a Session's,
  never the one that plays.
  Opt-in: without it no Sampler is made, no thread started and no code listed (`profile::made`), and the render's
  bits are the same either way; `dsp.profile` checks both, that a deliberately heavy function tops it, that an FFT's
  time is its caller's, and the parallel region's note.
- **Long renders and the cancel** (Measure.h, "How long a render may be"): a request's `longest` bounds its times
  (hold_seconds and the scenario's): `kMaxPlaySeconds` (10 s, the default) for a picture's render (view_sound,
  view_panel's `at`, compare's picture), `kMaxLongSeconds` (300 s) for a call that returns numbers alone
  (measure_output, a check, tune_dials, compare without its picture, trace: `scenario::forNumbers`, set before the
  rest is read), the tail `kMaxTailSeconds` (10 s) after either, a window both (`Request::endSeconds`); past its cap
  a value is refused in words that name it (a picture's naming the tools that take 300 s), a note's at + len and a
  move's at + over too (the Reader's `ends`), never cut there; and `scenario::fits`
  (resolve's last step, and compare's for B at its `b_rate`) refuses, at its rate, times past `measure::longestAt`
  (`kMaxRenderSamples` less a whole tail: 145 s at 192 kHz, `measure::latestTime`) and a render past
  `kMaxRenderSamples` a channel (310 s up to 96 kHz, 155 s at 192 kHz: `measure::renderSeconds`, an effect's test
  signals beside its own input, without a compile). A Session clamps to the same, so it never re-times a request the
  tools take. A Session's renders are cancelled as they go (`Session::setCancel`: the chat's `cancel_`, which the
  stop button and `mcpCancel` set, and `kRenderSeconds`, 170 s of the clock from the call's start, under the MCP
  deadline: `ChatAgent::renderCancel`, which also puts a long render's progress in the activity; `Cancel::start` and
  `Cancel::spent` are shared by the Sessions a call renders in, compare's sides (`Cancel::who`) and the checks' rates,
  and tune_dials' limit is its own `seconds` from its start): each asks every `kCancelSamples` samples, and once one
  is cut every render after it is cut at once until the next setCancel; measure() then measures nothing and its
  report says what was rendering, how far it got, after how long and how fast (`Session::cancelWords`, the limit
  named whole, and the render that spent it when the limit lands in a short one after it; `Result::cancelled`; no
  line of what was measured before the cut), as compare's error, a check's "not measured" and tune_dials' (its cut
  render left out; its first: nothing tuned) do. The analysis after a render is not asked: a cancel there lands at
  the next render or the end. The render stays on its caller's thread (the agent's worker, the link's one worker):
  the call's answer is its numbers, and the calls are the chat's one at a time (`busy_`); the live tools answer
  meanwhile. trace keeps its own clock (90 s) and stops pass 1 once its records pass 256 MB. `dsp.tune` LONG and
  `plugin.chat` LONG check it.
- **view_sound** (`dsp/SoundView.h`, `plugin/SoundPicture.h`): the render (the effect's with an input: a sweep, noise,
  an impulse, a sine or silence; the synth's the notes held, then released) analysed (the waveform's extent and level
  per column, a Blackman-Harris spectrogram on a log axis, the held part's average spectrum, a close-up) and drawn as
  a 1120 x 800 PNG on the worker thread with JUCE (its frames fixed in seconds as nearly as a power of two allows: the
  48 kHz sizes scaled by the rate, and `View::spectrumTop` on the 48 kHz frame's scale, so a picture at 96 kHz, as
  compare's at two rates, shows what one at 48 does; with a `window`, seconds of the render it draws for both products,
  the average spectrum and the RMS over it (a part shorter than a frame: one frame over it alone), the close-up at its
  middle, its edges marked and the waveform shaded); it goes in the tool's result like view_panel's
  (the chat's file keeps it; `saveState()` without pictures puts a line in its place) and its 480 px thumbnail on the
  tool's entry, which the chat draws under the row (the chat keeps the thumbnail). Through MCP a picture over `kMcpPictureBytes` goes as `chatimages::fit` makes it: the largest
  that fits of a JPEG and a scaled PNG, each sized from its own bytes (a spectrogram's PNG is several times its
  JPEG's, so sizing a JPEG from it sent a third of the width).
- **run_program's panel line** (`ChatHost::chatPanelFit`, EditorPort.h's `panelFitLine`, editor/Picture.cpp): after a
  run that plays a .gui file, one line when its interface is larger than the PANEL view at the editor's size (the
  session's, else the default; the chat open or not, at its width, and as narrow as the CODE view's explorer leaves it),
  "" when it fits: the check gui_sandbox's report makes (editor/GuiLayout.h's `guiFit`, `guiFitLine`, `guiNodeName`, in
  `FP_SANDBOX_SHARED`: how far the content reaches past the area and the elements not wholly in view at no scroll, four
  named), the area by the Panel's own arithmetic (`ui::panelViewArea`, which `gui.panel` VIEW holds to a laid-out
  Panel's, the chat open and folded, and at 1200 px, where the explorer narrows it; 853 x 612 at the default size, the
  sandbox's area). It lays out on the agent's worker, measuring text with FunkGui's font, which is the message thread's
  to make: `poll()` makes it once a program with a .gui file compiled (`readyFit`, `panelFitReady`, which also reads
  this Mac's chat and explorer widths there), a call on the message thread makes it itself, and another waits a quarter
  second for it, then says nothing. `plugin.reload` FIT checks it.
- **view_panel** (`editor/Picture.h`, `Picture.cpp`, `PictureDraw.cpp`; `Processor::chatPicture`): the turn's worker
  asks and the message thread draws (`Processor::poll`), as bgfx runs there: a headless Panel at the editor's size, its
  frame through FunkGui's `renderGpuPicture` (v0.23.0, `funkgui/gpu/GpuPicture.h`) where the build has bgfx
  (`FUNKGUI_HAS_BGFX`) and the platform GPU pictures, shaders drawn as on screen; on any other status SoftRaster (each
  shader FunkGui's striped placeholder), and the picture's text says so and why (`toString(status)`). With no editor
  open, the first picture starts bgfx headless and FunkGui holds it; `picturesIdle` (the processor's timer) lets it go
  ten seconds (`kPictureHoldSeconds`) after the last picture: a burst of pictures (Claude Desktop looking at one page
  after another) starts bgfx once (here the first 40 to 190 ms, by the load, the next about 25), and a host keeps no
  GPU context for a plugin nobody looks at. Never draw one off the message thread. `gui.shaders` PICTURE (against
  SoftRaster's image of the same frame) and `plugin.chat` VIEW check both paths (the fallback through
  `picture::forceSoftRasterForTests`). A state of Claude's choosing (`PictureState`, EditorPort.h; `values`, `at`):
  `ChatAgent::panelState` reads it on the worker, `at`'s render too (the program that plays, `chatRunning`, in a
  Session of its own with view_sound's settings (`dials_from` too, the result's first line saying defaults), offline,
  its `try_edits`, close-up seconds, `window` and `base_hz` refused: `MomentHooks` reads the meters
  from the first sample (its reader added in `RenderHooks::started`, before the first call), cuts the last call at
  its moment and keeps the Session Engine's meters 60 times a second of the render, at most 150, then its scopes and
  `Engine::dials`), and picturePanel draws it, only on the panel of the files it rendered (`PictureState::sources`,
  `cir::sameProgram`; else "look again"): the dials it sets through ports of the picture's own (`ShownPort`: the
  user's dials never move), and `Panel::showState`, the frames in place of the Engine's meters (held at the first
  until `playShown`, then one a tick, the last kept and its readouts made again), the scopes' samples, a field's taps
  and the levels over them (a live= mark of a field asked for shows before the audio thread plays the program:
  `GuiView::tick`'s `liveTaps`).
  Values name dials (plain, or an option's name), fields (every display, live= mark and shader input of that source
  or name) and in and out (dBFS); the text then gives what was drawn, what names nothing, and what each display shown
  reads. `gui.panel` STATE, `plugin.chat` PANELSTATE and `plugin.sandbox` SHADER (a shader's input) check it.
- **A scenario and trial edits** (`plugin/ChatScenario.h`; `measure::Request`'s scenario, `Timeline`, `RenderHooks`):
  measure_output, view_sound, each check (it keeps its scenario), tune_dials and compare take one `scenario` property
  (its schema one line, its details in the prompt's CHECKING YOUR WORK): a phrase, dial moves, wheels (a racing
  wheel's `buttons` a mask, its 7-bit parts sent in time order where they change, as the wheel sends them, and the
  start key's note while bit 17 is down: `Request::startKey`, which makes the report and the view take it as a
  phrase), host edits, the rate and the block ("host": `ChatHost::chatHostRate`, `chatHostBlock`, filled in before
  each render; "capture": the user's recording, through `scenario::captured` alone). A render plays host blocks as the processor does (the dials
  and the transport once a block, the events at their samples), so a phrase and the wheels give the same bits at any
  block size, and a moving dial, as in a DAW, does not; `dsp.tune` and `plugin.chatexample` check both at 64 and 333.
  measure_output, view_sound (and view_panel's `at`) and trace also take `dials_from` (current or defaults: `scenario::dialsFrom`; not in a
  check's render settings, which set_checks' own covers, nor tune_dials'), and compare its own `dials` (own, current or
  defaults), the result's first line saying defaults.
  `try_edits` apply to a copy of the editor's files with edit_file's rule (in measure_output, view_sound and compare's
  b), and the result's first line says nothing changed: these tools stay out of `kChangingTools`. compare
  (`ChatCompare`, `dsp/Compare.h`) renders two sources in two Sessions with one request; its "playing" is the
  program's own code, never the frozen copy, and it says so; its "previous_run" is what played before the last run
  that changed it (`ChatHost::chatPrevious`: the processor's `previous_`, set in `apply` when a run changes the
  sources, a netlist's drawing aside, a panel's edit too (the same program); before `poll()` takes a run, the `running_` it replaces; in memory only, none
  until a run has changed the program since the plugin opened or loaded a state: `apply`'s `loaded`, the
  constructor's Init and `setStateInformation`'s program, clears it). Its `b_rate` renders B at another rate
  (Comparison::twoRates), both rates at least 40 kHz (`measure::kMinRateAcross`, else refused: 20 Hz to 20 kHz on
  both sides, so an effect's sweep, its centroids' band and the picture's axis are the same): no sample lines up
  with another, so it compares the levels, the octave bands, the metrics and the detail's level per part, never the
  samples, gain or lag; the metrics a rate moves by their definition set aside and named (step; an effect's
  noise_gain_db, noise_lufs and impulse_peak_db: white noise and a one-sample impulse spread over the rate's whole
  band), and inharmonic_db under its -90 dB floor on both sides agreeing; an effect's input the sweep, noise and the
  impulse refused, and never a recording (its events are at its own rate). A `b_rate` equal to A's rate is the
  ordinary comparison, none of these refused; the picture places each spectrum column by its own view's frequency.
  A side `wav:` is a target sound (reference audio): a WAV, AIFF or FLAC on this machine by its whole path
  (`assetfiles::read`, with a sample's caps: 8 MB, 60 s; local files only, nothing fetched) or one of the program's
  samples by its name (`assetfiles::namesAFile` tells them apart alike on every platform: a '/', a '\' or a ':', or a
  leading '~', is a path, so a Windows path is never looked up as a sample; the refusals' example path is the
  platform's), its sound at the render's rate (`Asset::at`, resampled), cut to what a render may hold there (a
  picture's 10 s, saying so), handed to `measure::compare` as a recorded side (`CompareSide::recordedL`), so the other
  renders as long as it is; the result adds both spectra's centroid and correlation (`capture::spectrumOf`), no metrics
  (the file is no render), no dials' or files' line, and no thumbnail kept in the chat; refused: both sides without a
  program (two targets, or a target and the recording), `b_rate`, and `try_edits` when b is one. `plugin.samples`
  TARGET checks it.
- **trace** (`dsp/Debug.h`, `plugin/ChatTrace.cpp`): offline only, in a Session's own Engine, never the one that plays
  (`Engine::playingPatch` and `voices` are offline accessors). Its render is measure_output's (`scenario::renderOf`:
  the notes, the velocity, the times, the host's tempo, the wheels, the dials asked for and the scenario; the same
  bits). Pass 1 renders in 16-sample calls and reads every field
  of the voices that sound after each (`Patch::fieldMap`, `Patch::read`: inside the instances' memory as FMinus lays it
  out, never through a pointer), the alerts a call from `CallInfo` (never `Engine::takeAlerts`); a field runs away
  past 1e6 at a thousand times its size in its instance's first 10 ms of sound (Shared's and an effect's: the
  render's), so a state large from the start is not one, and a click is `measure::clicks`'s (the click metric's rule,
  so measure_output and trace agree; with the effect's test impulse as the input none is looked for, as measure's
  impulse render marks none: the impulse and its echoes are the response); pass 2 renders again in
  1-sample calls through a window of at most 4096 samples before each problem, and its output must equal pass 1's: when
  it does not, the program keeps state per call, and the result says so. The lines that write a field come from
  `fminus/Lang.h`'s resolution of each assignment (a heuristic). Deterministic but for its time limit (90 s) and a
  cancel (`cancel_`); find's result at most twenty lines; `dsp.debug` checks it on planted bugs. It never claims what it
  did not check: a problem a stop kept from pass 2 is "not traced", "the same bits" needs compared samples, a field
  non-finite before the first call is a note, not the problem, and with no field gone bad the verdict names the state the map
  leaves out (`Patch::unmapped`) before it blames a local. Its source "capture" is the user's recording replayed through
  the program that played it (`scenario::Captured::files`, the take's own, with its netlists' `pins`; the one that
  plays now when the take no longer knows it), read once for the files and the render (`scenario::resolve` with the recording).
  Its `export` (`plugin/ChatTrace.cpp`'s `exportFiles`; `TraceRequest::exporting`, `every`) writes pass 1's render as
  files on this Mac and names each whole in the result (the MCP server runs here, so a local client opens the path):
  a WAV (`measure::writeWav`: float32, whose data is the render hash's bytes, or 24-bit PCM, rounded, never dithered,
  clipped past full scale and counted), an effect's input as it went in beside it (`ViewRequest::keepInput`,
  `Sound::inL`), and with fields a CSV (`TraceResult::log`, `FieldLog`: a row after the call that reaches each multiple
  of `every`, a multiple of 16 rounded, about 1 ms by default; the voice as `voice` chooses, the watch's mean with all;
  `writeCsv`: a float to 9 digits, a double to 17, an empty cell where none sounds), then the CSV's rows evenly inline
  (`rows`, 32 by default, at most 64, within `kInlineBytes`: rows dropped evenly; after trace's cut, whole) for a
  client that cannot open the files. find looks for nothing unless asked. The folder is `rendersFolder()`: the user's
  Music folder's `<Company>/<Name>/Renders`, under CTest the probe's sandbox's `renders/`; each file is written through
  a temporary beside it and moved into place. The files are the user's: an export only adds files, never replacing or
  deleting one: a name given (`name`, tidied), else the time, names them, with a number after it while a file it would
  write is there (an effect's input file's name among them). So trace is no read-only tool to MCP, nor a destructive
  one (`kFaces`: its `destructiveHint` false); keep it so. An export that would take the folder past
  `kMaxFolderBytes` (2 GB) or leave the disk under `kKeepFree` (1 GB) is refused in words, and a trace cancelled (the
  stop button, Desktop's cancel: `cancel_`, in either pass) writes nothing. The WAV and the CSV say "up to where trace
  stopped" only when pass 1 stopped (`TraceResult::stoppedFirst`; a stop in pass 2 leaves its render whole). Never of
  the user's recording: a render that reads it (source or scenario "capture") is refused before it renders, as is
  `list` with it.
- **The render hash** (`dsp/Export.h`): every measuring report names what it rendered as bits, "Render hash: <16 hex>
  (SHA-256 of the float32 samples, L R interleaved, of ...)": measure_output's (`Result::hash`, `hashed`: an
  instrument's render of the notes held, the same render as trace's and view_sound's; an effect's counted test-signal
  renders in turn, the steady tone's aside; a whole report's only, so a check's and tune_dials' partial ones, `want`,
  spend nothing on it: about 190 MB of samples a second), view_sound's (`View::hash`), each over the render's own
  length (a window past its end renders on, and the hash leaves that out, saying so: `Timeline::plain`,
  `Sound::plain`; the same sound hashes the same whatever the window), compare's each side's
  (`Comparison::hashA`, `hashB`, a line after its own) and trace's (`TraceResult::hash`, of what pass 1 rendered). The
  bytes are a float32 WAV's data chunk's, so a build elsewhere, `shasum -a 256` or Python's hashlib gives the same
  digest. `dsp.debug` EXPORT and `plugin.chat` EXPORT check both.
- **The headless renderer** (`Source/render/Main.cpp`, `<Name>Render`): measure_output's Session and trace's export
  from the command line, so its report, its metrics (17 digits in its JSON), its render hash, its WAV and its CSV are
  the tools' to the bit (the CPU times aside); `plugin.render` holds it to that. It reads its settings with the tools'
  own readers (`plugin/ChatScenario.cpp`): a property a tool gains that the renderer should take is read there and
  named in its `kKnown`, and a change to what measure_output or trace's export renders reaches it with no change of
  its own. It drops the window for its trace (a window is the metrics'), takes no time limit (`TraceRequest::seconds`
  huge) and writes nothing when pass 1 stopped short (`stoppedFirst`: its records past 256 MB). What a request names
  and the program lacks is a note on stderr and in the metrics, never silent, since a script may keep no report: a
  dial (`Result::unknown`, or the controls and `Session::timeline`'s `unknown` with `--no-measure`: `unknown_dials`), a
  field pattern and the fields past the CSV's columns (`TraceResult::unmatched`, `more`). In a folder it leaves out
  FolderSync's read-only copies (its manifest's `copy` lines, `.fmsync`; and a host.fm of `Patch::hostSource()`'s
  text), before kMaxFiles. An effect's own input (`readWav`: formats 1 and 3, plain or WAVE_FORMAT_EXTENSIBLE) longer
  than a render plays of it (`longestAt` and the tail) is refused, never cut; a request may come through a pipe; a
  signal removes the `.part` it was writing. It has no samples (the plugin's session and projects keep them; no folder
  does: FolderSync mirrors text files alone), so a program whose compile read a `#pragma sample` line
  (`Recipe::samples` not empty) is refused, exit 1, naming them, after `--list-fields` and before anything renders:
  never rendered silent where it reads one. A renderer that loads samples needs a folder that holds them first.
- **set_checks** (`dsp/Goals.h`): checks are goals (a metric with min, max or a target and a tolerance) with a label
  and a render; `ChatAgent::evaluateChecks` measures them all (one compile, one render for checks that share one) on
  the editor's files and the plugin's dials after every run_program that compiles (the results end its result: the
  count and each check that fails; the card shows every one) and
  when a reply ends a turn in which a tool changed something (`kChangingTools`, a result that is no error); a failure
  there adds the results as a user text message (saying the dials as they are now only when every check takes
  those) and the turn goes on, at most `kMaxCheckNudges` (2) times a turn. One
  `ChatEntry::Kind::checks` card a turn, brought up to date in place; its Clear calls `ChatPort::clearChecks`. A check
  takes the dials it does not name from the plugin as they are when measured (the default: tune_dials' and set_dial's
  results show in the checks), from its snapshot (set_checks' `dials_from`: the dials when it was set) or the
  defaults (`measure::DialsFrom`); a FAIL line names the dials it took that are off their defaults (or the snapshot),
  and one at the defaults says the dials as they are do not affect it. A check whose dials or scenario moves name a
  dial the program lacks FAILs for that, whatever its metric gives (`measure::Result::unknown` into `Goals::evaluate`),
  and run_program names each check its new declarations left naming a dial the program before had (`orphanLines`),
  and apart each from a snapshot that holds one (it does not fail: measured without it, a new dial at its default).
  The chat keeps `checks_` (its file, `saveState`) in set_checks's own shape (`checkVar`: `dials_from` and the snapshot with each check),
  and set_checks without `checks` lists them so, numbered, `dials_from` on every one, current too (an item's own
  `dials_from` and snapshot stand for the call's, so a listed check given back is the same check; the result says
  how each check it set takes its dials, from the checks, not the call); `mode` merge (each item replaces the check
  of its `index`, else of its label, case aside, else is added; one replaced under another label is named with both,
  so a stale index shows) and remove (by index or label; an index whose check has another label than the item's is
  refused) change them all or not at all, and a label two checks share needs the index. A listing changes nothing:
  it never sends a turn on, nor does tune_dials' sweep (it sets nothing) or write_file's `to` (an export). Never let a
  check nudge without a cap: each
  nudge is a billed request.
- **The planner** (`setPlanner`, chat.json "planner"; ChatAgent.h "A planner"): `planTurn` sends a message's
  history to the planning model through `forAnotherModel` (no thinking blocks, which are bound to the builder's model
  and prompt; no compaction or server-tool blocks; citations dropped; no field another provider left on a call,
  `providers::forAnthropic`) with `tool_choice` none and the tools defined
  (the history's tool calls stay valid), a brief after the user's words; the plan streams into a
  `ChatEntry::Kind::plan` entry and is appended to the user's message before the builder's first request (never to
  a message already sent). `ask_planner` sends the history up to the reply that asks. A failing planner leaves the
  turn to the builder with a note. `plugin.chat` PLANNER checks the requests' shapes, `plugin.providers` BACK a
  planner's request after another provider's turns.
- **MCP** (`ChatAgent::mcpTools`, `mcpGuide`, `mcpGuidePart`, `mcpLive`, `mcpCall`, `mcpCancel`; `plugin/McpLink`;
  `Source/mcp`): Claude Desktop drives the plugin with the chat's own tools, so a tool changed or added reaches it
  with no change elsewhere; ask_user and ask_planner stay the chat's. A new tool gets a line in `kFaces`
  (ChatAgent.cpp: its title in words, read-only, destructive, and the guide part its notes go in, "start" unless
  said), and its description's first sentence (or first paragraph, when it has several) must stand alone: that is
  all MCP's copy keeps (tools.json sits in every Desktop conversation), the rest goes in the guide's THE TOOLS: WHAT
  THEIR SHORT DESCRIPTIONS LEAVE OUT, in that part. MCP's copy of the schemas also leaves out a property's description
  an earlier property of the same name has word for word (`mcp/McpSchema.h`'s `dropRepeated`: a goal's words in
  tune_dials are set_checks'; the guide says so), except a description with a number in it (a range, a default, a
  cap), which every copy keeps: Desktop may load or switch on one tool without the one that described it first. The
  server runs the same rule over its whole list once it has added `instance`, so that one is described on the first
  of the plugin's tools alone. The annotations say only what MCP's defaults do not (`mcp::annotations`): readOnlyHint
  when true, destructiveHint on a tool that writes alone, openWorldHint false, and no title (the tool's own is MCP's
  since 2025-06-18; the server copies it into the annotations for a client of 2024-11-05 or 2025-03-26). **The tool
  budget** is on what Claude Desktop receives: the server's tools/list reply as it goes over stdio (one line: the
  chat's tools as MCP has them, each with the server's `instance`, and the server's own), at most 24,000 bytes for
  each product, the synth's size when it was first measured so (23,963 bytes; the effect's 23,124) rounded up to the
  next 100, so that what every Desktop conversation carries does not grow: a feature comes as a property or words on
  a tool that has room, or a trim elsewhere pays for it (as these did: `instance` in a few words, described once, how
  to name one in the server's instructions and the guide's start; an instrument's render settings in an instrument's
  tools only; tune_dials' metric a plain string the tool checks, set_checks' the enum; the annotations' defaults left
  out; the longest descriptions in fewer words: the synth's 20,959 bytes after those trims, the effect's 19,565;
  22,012 and 20,638 with the measuring tools' properties added since; 22,192 and 20,818 with write_file's `path`;
  22,232 and 20,858 with the target named in `instance`'s and list_instances' words; 22,670 and 21,296 with the
  projects tool, its 544 bytes paid in part by shorter first sentences of add_library_file, view_panel, tune_dials and
  compare; apart from those, 22,599 and 21,225 with run_function, write_file's files, from and to, and
  measure_output's profile, paid in part by shorter first sentences of add_library_file, edit_circuit, get_status,
  tune_dials and compare, whose words moved into their guide parts or went; 23,343 and 21,969 with all of them, the
  shorter kept where both had cut a first sentence; 23,400 and 22,026 with the guide's cheatsheet part, waveguide.fm,
  level_wobble_db and compare's wav:path, capture's marks costing none: they are past its description's first
  paragraph, all that tools/list carries, the rest the guide's). `plugin.mcp`
  checks, and prints each product's size and its slack (and tools.json as the link writes it, pretty-printed, for
  comparison: until then the bound was 26,000 bytes on that). Each product has its own server, extension and socket
  folder.
  The server is juce_core only; it finds the instances by their sockets (`<n>.sock`, the lowest number free: its
  number's name, "Effect 2" or "Synth 2", until the user names it, `McpLink::setName`, kept with the session as the
  state's `<MCP desktopName>`: `McpLink::userName`, the user's own, so a link that cannot listen loses none; a rename
  tells the host the session changed, a restore does not) and falls back on the tools.json and guide.md the newest
  left. It is for someone who never sees a tool: its instructions keep tools and ids from the user, each tool has a
  title in words, an instance is matched by its name, its number (and its number's name, which still reach a named
  one, as does list_instances' own "Bass (Synth 2)", and its number counts among a named one's words), track or project words,
  and an unclear one makes Claude ask in the user's words; the prompts (Make a sound, Improve the sound, Explain the
  sound, Fix what I hear) name sounds and tracks, never tools, are text only, carry the plugin's get_status when there
  is a target, and one whose tools the plugin lacks is not listed. **The target** (`mcp/McpTarget.h`): a call that
  names no instance goes to the one the user chose in a plugin (Use with Claude Desktop, `target.json`, whose token
  must be that instance's card's: a number taken again is never the one chosen), else to the only one open; with
  several open and none chosen it is refused with them described, never guessed (no "used last": one server serves
  every Desktop conversation, so a guess made for one is wrong for another, and a call carries no conversation, so no
  choice per conversation can be had). list_instances says which is the target. Each link writes its card (name,
  track, project, process) when it changes and reads the others' about once a second, for the plugin's CLAUDE
  DESKTOP; the choice goes when that instance does. Cards and target.json are written beside and renamed over in one
  step (`mcp::replacePath`: rename(2), MoveFileExW retried while a reader holds it; never JUCE's `moveFileTo`, which
  deletes first), and a choice whose write failed is not shown as made. `plugin.mcp` checks both sides (TARGET,
  RENAME, the indicator), `gui.desktop` the plugin's face of it. The
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
  progress and every render read as it goes (`Session::setCancel`, above), and a tune cancelled so applies nothing)
  or drops one waiting; a connection closing cancels its
  calls, and the link going (the plugin closing) cancels and waits for the one running.
- **A live tool** (`ChatAgent::mcpLive`, one line a tool: get_status, listen, and capture's status and stop without a
  picture) runs on the link's poll thread (listen on a thread of its own: `mcpLiveWaits`, below) while a call may be
  running on its worker, without `busy_`, never refused. Such a tool's handler is called concurrently with a running
  call (a tune, a render, a compile) from another thread, so it reads only atomics, seqlock copies (`Engine::Meters`) or
  data under its own mutex (ChatHost's locked getters count: the draft under `programMutex_`, the Compiler's status, the
  dials under `dialMutex_`, as get_status reads them); it never touches the program (a Patch, a Session), the editor,
  the chat's history or `busy_`, never waits for the message thread or the worker, and returns in milliseconds. A tool
  that cannot keep that is not live.
- **listen and get_status's `since`** (`plugin/Live.cpp`, live: their reads keep the contract above). listen is a call
  that takes seconds (`ChatAgent::mcpLiveWaits`), so the link runs each on a thread of its own (`McpLink::listen`, four
  at once at most; a cancel or the connection closing stops it, the link going cancels and joins them), never on the
  poll thread; it raises the meters' reader count while it listens (`ChatHost::chatListening`, lowered on every exit)
  and polls `ChatHost::chatLive` about 25 times a second, counting a frame's fields under the panel's displays only
  when the frame is of the panel's program and tap table (`LiveRead::tapTable`, as `ui::TapsShown` reads them: a
  panel's edit moves fields between indices), the newest such panel's alone (`plugin.reload` LISTEN); a cancel that comes before it starts is kept (`mcpStopped_`,
  every one, not only the last). get_status ends with a token of the processor's journal (`plugin/Journal.h`: about 256
  changes, drags merged, under its own mutex; what Claude Desktop does itself is not kept, `ChatAgent::mcpWorking`; each
  token handed out is a new one), and `since` lists what changed after it, in the third person ("the user moved", "the
  host", "the plugin's own chat": a "you" reads as the Claude reading it), a dial's moves as one line, its net change
  (first -> last, at its first move's place, with who and how many times) up to a change that sets dials with no line
  of each (`Journal::dialsNote`: a run that compiled, a version opened, a preset applied, a fresh start, a state
  loaded; a move after it is a line after it, never merged across it), the files as diffs (`dsp/TextDiff.h`)
  against their texts at that token (`mcpMarks_`, the newest 16 tokens', under `liveMutex_`). The session keeps those
  tokens with their files' hashes and lines (`ChatAgent::mcpMarksToKeep`, Live.h's `KeptMark`, the state's `<MCP>`):
  in the plugin opened again (a new journal, which knows no token from before) they are `mcpKept_`, and `since` with
  one of them gives the whole state, then which files changed since it (by hash, with read_file's versions; not how).
  One server may serve several
  conversations and its calls name none: what is kept for Claude Desktop is kept by token, never as one record of what
  "it" saw, which one conversation's look would move for all. The two Claudes keep their own records: an MCP call never
  marks the chat's `seen_` (`markSeen`), so the plugin's chat is sent a file Claude Desktop wrote; a chat turn never
  runs beside a call of Claude Desktop's (New chat and a session load keep `busy_` while one runs). Every other MCP
  result starts with a line when something changed since the last MCP call (`ChatAgent::mcpHint`, one cursor for the
  instance: how many changes, "The user changed" only when each is the user's, the first three as the journal says
  them, their values with them, and how many more; its token is
  handed out, so a drag or typing that goes on after it is a line of its own, never merged into one it named); a read of the server's own (get_status's `peek`, the GUI sandbox's, in no schema) moves neither it nor
  the tokens. The journal learns who: the editor's ports are `GesturePort`s (a dial moved between polls with one of them
  is the user's, else the host's; a MIDI control learned writes through them too), `setDraft` is the editor's and `chatSetDraft`, `chatRun`, `chatSetDial` the plugin
  chat's (or Claude Desktop's); `poll()` compares the dials, the bypass and the transport once a poll and takes as seen
  only the dials it set itself (`ownDials_`, `syncOwnDials`: a run's, a version's or a preset's, Claude's), so a move
  made meanwhile is journalled. The tools' dial lines say each value as the panel does (`dialText`, Controls.h: its
  unit, its option's name, the number after a scaled one). `plugin.live` checks both tools, `dsp.meters` LISTEN the
  fields, `plugin.chat` SEEN the two records.
- **MCP results** stay small: a picture is at most `kMcpPictureBytes` (75,000 bytes, 100,000 base64 characters:
  the picture as it is when it fits, else the largest that fits of a JPEG and a scaled PNG, each sized from its own
  bytes, `chatimages::fit`), the text after it saying what was sent;
  view_panel and view_sound take MCP's own `detail: "high"` for the whole picture. The plugin's own chat keeps its
  pictures as they were. The guide is the system prompt as `ChatAgent::guidePrompt` has it: the same text but the
  lines of How you work only the plugin's chat needs (ChatPrompt.cpp's `kWork` marks them: its `<plugin_state>`, the
  planner, web search, ask_user, its panel's markdown), which come last, under WHAT ONLY THE PLUGIN'S OWN CHAT HAS
  (`mcp::guide::kChatOnly`), in part "all" alone; a line of How you work Claude Desktop has no use for is marked so.
  It comes in parts (`mcp/McpGuide.h`, cut at ChatPrompt's headings at run time, in the plugin and from guide.md in
  the server): "start" (working from outside, the contract, the host with host.fm in short (how a program reads it
  and its module's declarations on a line, made from THE HOST's `<host_file>`), FMINUS, DSP PRACTICE, CHECKING YOUR
  WORK, the tools' notes, the other parts named), large-program (with THREADS), interface, library (a line a file),
  library:<file> (its header), library:<file>:code (its whole text, adding nothing to the editor), library:host.fm
  (host.fm whole, THE HOST's `<host_file>`; a part that is none names it), debugging (no heading of the prompt's: the
  notes of listen, capture, trace and compare, whose uses CHECKING YOUR WORK names, and run_function's), examples, projects (with the projects tool's notes), circuits
  (CIRCUITS: .cir NETLISTS, the prompt's last section; a guide.md from before it cuts as it did), cheatsheet
  (`mcp::guide::kCheatSheet`, FMINUS AND DSP AT A GLANCE: `ChatAgent::cheatSheet`, ChatPrompt.cpp's, after the tools'
  notes and before `kChatOnly`: the contract of this kind, the language, the DSP numbers and pitfalls, the library a
  line a file; MCP's alone, never in the chat's prompt; a guide.md from before it has none), all. A heading
  renamed in ChatPrompt.cpp is a part lost: `plugin.mcp` GUIDE fails. Its start is at most 9,200 tokens for the synth
  and 8,100 for the effect (bytes / 4: 8,701 and 7,517 now; `plugin.mcp` prints them).
- **The GUI sandbox** (`gui_sandbox`, the server's own tool, `mcp/Main.cpp`; its helper `<Name>GuiSandbox`,
  `Source/sandbox/`): Claude Desktop designs a panel.gui and sees it drawn as the plugin's PANEL view draws it, with no
  plugin open and nothing of an open one touched until the user asks to apply it. Its rules:
  - **The same pixels.** The helper draws with the editor's own code: `editor/GuiView` (the interface's items made, laid
    out, fed and drawn: the Panel uses it, so a change to the PANEL view's drawing is the sandbox's too), `GuiItems`,
    `GuiLayout`, `Looks`, `DisplayItem`, `picture::draw` (the GPU, else SoftRaster), over the program's card's colour,
    on a frame of whole pixels at 1.5 (an odd size is drawn a px larger and cropped: a scale off 1.5 shifts the far
    side). Never reimplement drawing in `Source/sandbox`; move what it needs out of Panel.cpp instead. `plugin.sandbox`
    MATCH holds the sandbox's picture of the prompt's example to view_panel's, to the pixel but the displays and live=
    marks (demo values: a steady tone, fields moving).
  - **No plugin code.** The helper links the files `FP_SANDBOX_SHARED` names and FunkGui; `Source/sandbox` includes
    nothing of the plugin, the audio side or the compiler (`lint.layers`), so no FMinus and no LLVM (about 4 MB). The
    dials are read, never compiled (`Lang`'s symbols: the main module's params and their `[[fminus::range]]` and
    `[[fminus::default]]`, as `Patch`'s `controlOf` makes controls of them), or Claude's list (its `unit` only what a
    blank panel writes as unit=: as in the plugin, a dial shows a unit from the .gui alone); with no program, the
    fields the displays name are mocked as floats; shaders built with the installed plugin's shaderc (or
    `<prefix>SHADERC`), through the plugin's cache (its disk folder too), else a placeholder and a note (and render
    says so). render's theme is the user's (the
    product's preferences, read again each time) unless asked. render's `values` name dials and, as view_panel's do,
    fields and in and out (`View::setShown`: over the demo meters); the two tools' `values` share their words
    (`mcp::kPanelValues`), so the server's list gives them once.
  - **The process.** Sandboxes by number: Claude Desktop's conversations share the server, so each open makes one
    (Sandbox #n, in every answer; numbered on from a random start, so a number from before the server started again
    names none) and a call naming its number (`sandbox`) acts on that one, whatever another conversation opened since;
    apply needs it. The helper holds one at a time; the server keeps the newest eight as the
    helper last said they are (each reply's `state`, render's `area`) and sends the one a request is for with it
    (`load`), as it does to a helper started again (one that died, was killed, or was ended idle), so the helper never
    holds what the server does not know, and an open that fails leaves the sandbox as it was. Requests one at a time,
    queued, each with a deadline (30 s, `<prefix>SANDBOX_DEADLINE_MS`): past it the helper is killed and the call says
    so. The server owns the idle time: after ten idle minutes (`<prefix>SANDBOX_IDLE_MS`) it closes the helper's stdin,
    so no request crosses the helper's quitting (the helper's own twelve minutes are a fallback); a helper busy when
    the server exits is killed. Nothing blocks the loop: the pipes are non-blocking, with an outbox (as the instances'
    sockets), and an ended helper is reaped later (`waitpid` with `WNOHANG`, `sbReap_`); the helper holds none of the
    server's descriptors (`POSIX_SPAWN_CLOEXEC_DEFAULT`); its stdout is the protocol's alone (it moves fd 1 to stderr
    first). `<prefix>SANDBOX_TEST_HANG` (a request it never answers) is the probe's.
  - **Read only but apply.** open reads an instance's files through get_status (files "all" and `peek`, the server's
    own: live, no write, and nothing kept of the look, no token, no mark, `mcpHint`'s cursor where it was, so what the
    user changed still starts Claude's next result), a file or folder the user named (read only, 256 KB a file, sixteen
    files; a folder's `isProgramFile` kinds, program.fm first, the folder sync's copies left out as the renderer leaves
    them: Sources.h's `syncCopies`, `programFirst`); apply alone writes, through the link's write_file and run_program, into the program's own .gui file (the
    first .gui, else panel.gui), and only when the user asks (the guide says so), its answer starting with write_file's
    line of what the user changed. The plugin's own chat has no sandbox. Its notes are in the guide's interface part
    (`ChatAgent::mcpGuide`'s `kSandboxNotes`); a change to the tool changes them and `FP_MCP_VERSION`.
- **capture** (`plugin/Capture.h`, `plugin/ChatCapture.cpp`, `ChatHost::chatCapture`): the recorder of what the host
  plays. `Processor::hostBlock` and `play` call its audio side (begin, the input before each Engine call, the output
  after it, each event the Engine gets, end); a recording crosses by atomic pointers, armed announce-then-claim and
  handed back for `Recorder::housekeep` (from `poll`) to free, as the Engine's programs do. The audio thread only copies
  into rings the arming thread allocated and touched, and never frees or waits: a full ring wraps (last) or ends the
  recording (next), with a flag. A reader copies what was published and leaves out what may have been overwritten
  meanwhile, so stop and status (live) never wait for the audio thread. While a recording is armed the program's own
  code plays (`Engine::setOwnCode`: the frozen copy is dropped, none is taken, the freezer waits), so a replay of a take
  can match it bit for bit. The user's audio and MIDI stay in memory: never on disk (trace's export refuses a render
  of it), in the session or a project, never returned as samples (the tool reports numbers; a render hash gives none
  back), no picture of it among the chat's kept thumbnails; freed on forget, the next arm, or ten minutes without a
  call. So capture is no read-only tool to MCP, and a destructive one (`kFaces`: an arm changes what plays, a new arm or
  forget lets the take go). A take replays (`plugin/CaptureReplay.h`, through `scenario::captured`):
  a `measure::Request` with its blocks (in the processor's parts, `Request::part`), events, transport, its dials as the
  processor sent them (`Request::frames`, by name for another program), the Engine's clock (`Request::clock`,
  `Engine::setClock`: the voices' grid phase) and the wheels at its start (a racing wheel's control changes those off
  rest, `wheelcc::restOf`; without the host's transport, the Engine's own song position: `Engine::setBeat`; a
  Session's `Engine::restart` puts the wheels, the pedal and the position at rest); the first replay null-tests it
  once (a compile and a render of the program that played, with the samples as they are, set after `replayOf`, which
  starts from an empty Replay; off the live calls) and the recorder keeps the line for stop
  and status. restart (mode next) recompiles what plays first (after a run on its way) and waits for that program to
  play alone. Each block keeps the Engine's steals and dropped voices in it (`Meters::steals` and `::dropped`, the audio
  thread's own publish, read at the block's start and end). stop and status stay live but for `picture` (drawn on the
  worker). The user's marks (`Recorder::mark`, `capture::Mark`): while it records, the status bar's REC (its seconds,
  the marks) and Mark (`ui::Panel`'s `markItem_`, Cmd-Shift-M; `EditorPort::recording` and `markRecording`, the
  recorder's `glance` a try-lock that never waits for a call copying a take) mark the moment: what the audio thread
  announced last, under the recorder's mutex (Mark waits for a call copying a take: a mark late by that wait beats one
  refused), into the ring's own list (never the audio thread's), numbered in order, its number returned for the
  status's notice (never read back from a glance), `kMaxMarks` at most in what a take could still hold (a long `last`
  recording lets go of the oldest once they have left the ring, counted); a take keeps those inside it at their
  samples and numbers (`Take::marks`, `marksBefore`: those before the part kept and those let go), the report lists
  them with their nearest recorded moments, and a scenario's from and to take one by its name ("m2":
  `Request::captureFromMark`, `captureToMark`, resolved against the take in `scenario::captured`). `plugin.capture`
  checks it (MARKS); `gui.dump` RECORD the notice, its keys and its PNGs.
- **tune_dials** (`dsp/Tune.h`): Nelder-Mead over the named float dials' normalised ranges, from the dials as they
  are and a Halton spread, restarted when it settles with a goal unmet, until every goal is met, the budget (150, at
  most 400 renders) is spent, its time limit passes (`seconds`: 120, at most 170, under the MCP server's deadline)
  or the stop button cancels, each also within a render (`TuneRequest::cancelled`, the render cut left out);
  deterministic but for the time limit. The best values go on the dials through
  `chatSetDial`, unless Claude Desktop cancelled the call (`mcpCancel`): then the dials stay exactly as they were.
  Its `sweep` (`TuneRequest::sweep`, at most `kMaxSweep`, 32) searches nothing and sets nothing: the one dial named
  (of any type; a value outside its range, or an int's or a bool's not whole, refused) at each value in turn, the
  goals' metrics at each in a table, a row a value; a sweep's goal may be a metric alone (`invalid`'s `bare`,
  `judged`), and the judged ones mark each cell met or not and name the values that meet them all, or the nearest
  (the least weighted penalty); a metric a render leaves unmeasured is named with its first reason and how often.
- **The system prompt** (`ChatPrompt.cpp`) teaches Claude FMinus, the contract, how to build a large program, the
  `.gui` file's language (THE INTERFACE: GuiSpec.h; the `#pragma panel` lines only as the older way), the library
  (every file's header, from Library.h) and the examples, and shows a program built from the library for the
  product's kind (`ChatAgent::promptExample`: the program, its `panel.gui` and the library files it includes), which
  `plugin.chatexample` compiles and plays: a change to the contract (Patch.h), the panel's lines, the language's
  rules, a library file's header or the examples changes it too, and the example must keep compiling with no note.
  FMINUS, THE LANGUAGE says what FMinus at its pin compiles, with one example of its newer constructs (an enum's
  sizes, a top-level table, a switch, noise from a count), which `plugin.chatexample` compiles alone and runs: a pin
  move that changes the language changes the section, within the start part's token bound (`plugin.mcp`).
- **Pictures** (`attach`/`detach`, `ChatImages.h`): read on the agent's job thread (`ChatAgent::queueJob`), kept as
  sent (the file's own PNG or JPEG bytes when they fit, else encoded again) with a thumbnail (RGBA8, and encoded for
  the session); sent as base64 image blocks before the message's text; six a message, twenty in the history. One
  past `kMaxPixels` (100 megapixels) is refused by its header's size before any decoder sees it (a PNG's IHDR, a
  JPEG's first SOFn, a GIF's first image descriptor and its screen, read step for step as JUCE's loaders read them;
  ImageIO's properties for the rest on macOS), as is one of those three whose header says none (`chatimages::decodes()` counts the decodes). A
  message sent while some are read shows at once and goes with them: the turn's worker waits for the reads asked
  before it (`takePending`: Stop ends the wait, `kPictureWaitSeconds` bounds it), never the panel. The
  editor draws thumbnails through FunkGui's image registry (`funkgui::images`, v0.20.0): `Panel::syncPictures`
  registers each picture's id while `chat_` holds it, counted across Panels (Picture.cpp makes one), so the last to
  let go removes it.
- **Web search** (`setWeb`, chat.json "web"): the API's server tools web_search and web_fetch (20260209) go in the
  request on a turn whose model takes them (`turnWeb_`), and a turn whose tools differ from the history's drops its
  thinking (`historyWeb_`, in the chat's file as "web"). Their blocks (`server_tool_use`, the `*_tool_result`s with their
  encrypted parts, text with `citations`) go back as they came (`MessageBuilder` keeps them); `pause_turn` sends the
  history again with nothing after it. The sources listed under an answer are the citations' pages (the docs ask for
  them).
- **ask_user** pauses the turn: the reply's other tools run and their results wait in the history; the question's
  entry stays open (`ChatState::asking`) until `ChatPort::answer` (the labels clicked) or `send` (the user's own words,
  after a result that says so), and either starts the next turn with the question's result first in the user
  message (`missingResults`, `deliver`: tool results before any text, as the API asks). One ask a reply; the session
  keeps an open question ("asking"). The view draws it as a card (`ChatView`: ShapeKind::option, ActionKind::option and
  submit).
- **edit_circuit** (`ChatAgent.cpp`; the CIRCUITS guide part's THE SCHEMATIC teaches its grammar): Claude's edits of a
  netlist through the schematic's own operations (`cir::parseOp`, `cir::apply` on a copy, all or none, with the
  netlist's session pins, `ChatHost::chatSchematicPins`), written by `ChatHost::chatSetDraftIf`: only while that file's
  text is still the text the tool read (under `programMutex_`, the other files left as they are now), so a gesture of
  the user's (which the view gives the processor at once) is never overwritten; the commands run once more on the new
  text, and a second clash fails ("pedal.cir changed while drawing: try again"). Its result: what each command did,
  the netlist's lines changed (its `*@` lines aside), `circuits::Generator`'s errors and warnings on the new text, the
  whole text when at most 4,000 bytes (then seen); a longer one is seen when the result lists every line changed (60 at
  most) and the chat's Claude had seen the text it edited (`carrySeen`, as edit_file). `plugin.chat` CIRCUIT plants the
  gesture through `ChatAgent::setCircuitWriteHookForTests`.
  view_panel's view "schematic" is `pictureSchematic` (`editor/Picture.cpp`: a `SchematicPicture`, a Panel of its own
  that holds no editor Panel, so it owns no probe and reads or writes no file view; the drawing at 100 %, labels
  always). write_file of a `.cir` whose text has no `*@` line keeps the old ones and says so.
- **Claude's text is the editor's** (`asEditorHolds`, ChatAgent.h): every tool that puts Claude's text in the draft
  (write_file's content, files and from, edit_file's new texts, edit_circuit's values and names) puts it there as
  `TextEditor::sanitise` makes it, saying what changed, so the draft, a version, `seen_` and the tab are one text and the
  editor's next push changes nothing (`plugin.chat` ASCII).
- **read_file and edit_file** (`plugin/ChatFiles.cpp`): every read and write says the file's line count and version
  (its text's hash; the newest 16 versions' texts kept, `fileVersions_`). read_file gives a file whole, or a range
  (`from`, `lines`) or the lines holding a text (`find`, two around each), numbered, and a part read marks nothing seen;
  `find` with `regex` is a pattern (`dsp/Regex.h`; refused in words when it is none it takes) and with no name searches
  every file, in the tabs' order (each file's line count, version (kept, for `expected`) and lines, at most 40 shown in
  all), in find's own case first, then in any; a pattern's steps are at most 20 million a call (`kSearchSteps`, about
  0.1 s: the link runs its calls on one worker), past which the search stops and says where and what it did not search,
  and its compile is bounded too (`Regex::compile`: a count of what writes nothing writes nothing; too large refused);
  an edit marks the file seen only when the chat's Claude had seen the text it edited (`carrySeen`), so a file read in
  part, or changed by the user since, goes whole with the next state.
  edit_file replaces one passage or a list (`edits`), in order, each occurring once in what the ones before it left,
  all or none, in one write (one undo step); a passage that does not occur gets the closest one back, numbered, and
  what differs (a line of old_text that may be part of a file's line, its first or last, scores by how much of it that
  line holds). It writes through `chatSetDraftIf` (the text it read, the others left as they are now: what the user
  typed meanwhile is never overwritten; the edits run once more on the new text), and with `expected` (a version) only
  while the file is still that version, else nothing, saying whose change it was and showing the lines changed since
  (the version is compared, not kept, before that lookup, so the oldest kept is still found): Claude's own only when a
  read or write of Claude's gave the text now after it gave that version (`FileVersion::given`), so the user's undo
  back to a text Claude saw before is the user's, and what a refusal alone showed counts as no call that gave it. Its `files` does that for
  several files at once (a property, not a tool: the tool budget), in place of `name`: a list of `{name, edits or
  old_text and new_text, expected}` (one sent as a JSON string is read as the list), or a unified diff (its
  `---`/`+++` headers name the files, git's a/ and b/ off; a hunk is an edit whose old lines, context and `-`, are found
  where a line starts, the occurrence nearest its `@@` line as the hunks before it moved it, its header's counts
  unread, a blank line in it a blank context line, at most 256 hunks a file; nothing that could be part of the change
  is passed over: a context, `-` or `+` line outside a hunk is refused, naming the line that ended the hunk (a context
  line that lost its space, or indented with a tab), so is an `@@` line that is no hunk's header, and a diff that
  creates, deletes, renames or copies a file, its git header-only blocks included, is refused, naming the tool that
  does it); all or none across the files (one bad edit, one `expected` that no longer holds: nothing written to any),
  in one `chatSetDraftIf` of every file it read (one change of the draft, so one merge, and a journal entry a file).
  The editor makes a merge that changed several tabs' texts one undo step across them, a session loaded over open
  tabs included (`FilesItem`'s undo groups, newest last, at most 32: each member a tab's serial and its place in the
  tab's undo history, which FilesItem counts: a step a merge or Play this circuit makes, each undo and redo of the
  keys, the Schematic view's buttons and menu (`SchematicView::Hooks::step`) and Play's Undo). Cmd-Z in a tab at a
  group's step undoes it in each of the group's tabs at it, and Cmd-Shift-Z or Cmd-Y in one with it undone redoes it
  so, however far the tab went back and came again, so each such change is one step, one after another; a change of a
  tab's text it did not count (typing, a gesture, Format) takes the tab out of every group, and a new step where it
  could redo out of the groups whose steps those were; a fresh start or a load ends them all.
- **run_function** (`plugin/ChatFunction.cpp`, `fminus/PatchCall.cpp`: `Patch::call`, `adopt`, `layout`; FMinus's call
  by signature, `Program::callable` and `call`): one of the program's functions called by name with Claude's arguments
  (a number for a float, a double or a whole one for an int, true or false for a bool, an array or `{extent, values}`
  for an `int*`, `float*` or `double*`: a buffer of its elements, its extent or the call's largest int argument,
  whichever is most (FMinus checks no access through a pointer parameter: C's (buffer, n) stays inside it), then as many
  zeros again (`Patch::kCallGuard` at least), given back after the call, and said when the function wrote into those
  zeros; an int past `kMaxCallElements` beside an array refused; a module's pointer only as a method's own), checked
  against the declaration in words of FMinus's form before anything runs, its result or its error in words, and a
  method's fields that changed (`fieldMap`'s, before and after). Never on the audio thread, and never through a Patch
  that plays: the loop budget and the bounds record are a Program's, which a call from another thread would share with
  the audio thread's calls (setLoopBudget is the Program's), so the tool compiles a Patch of its own on its thread (the
  worker, or the link's), whose call runs within `kInitBudget` (about a second) with the bounds checks, in
  flush-to-zero; a call that stops (`callStopped_`) leaves its Patch calling no end. A method runs on instance 0 of that
  compile (the main module's: init run at the host's rate, the dials set by name as they are), on Shared's, on a field
  the main module holds by value, by its path (`filter.tick`, `osc[2].env.next`, `shared->x.f`), or on an instance of
  its own (`Svf::tick`: zeroed, its params at their defaults). `on: playing` compiles the program that plays with its
  pins (`CompileStatus::circuits`, of one status read: its `program`, and its `revision` read again after the files)
  and, once `Patch::call` has checked the call and only when it runs on the state (its `onState`: a method of instance
  0, Shared, Host or a field of theirs; a function or an instance of its own asks nothing), asks the audio thread for a
  copy of its state (`ChatHost::chatSnapshot`, `Engine::askSnapshot`: one `Snapshot` at a time, into buffers the asker
  wrote whole first, so the audio thread's copy takes no page fault, claimed by an exchange at a block's end after
  `publish`, the effect's instance or the newest sounding voice's (the taps' rule), Shared and Host copied when their
  sizes are the asker's, whether a voice sounds or not (`noVoice`: Shared and Host alone, which a call on Shared or
  Host takes; `onState` is told whether the call needs the instance, and only such a call is refused then), at most
  `kMaxSnapshot` bytes; nothing allocated or locked there, a relaxed load a block while
  none waits; the asker waits `kSnapshotWaitMs` and takes the ask back with a compare-exchange when no block comes,
  waiting for `done` when the audio thread claimed it first), compares `Patch::layout`'s hash and `Snapshot::program`
  with that status's (a run the audio thread has not taken yet, laid out alike, is another program: refused, call it
  again), and `adopt`s it into its own Patch: every pointer of the copy but the main module's Shared* and Host* (and
  Shared's Host*, all pointed at its own) and a sample's `data` (`SamplePlace`: pointed again at the sound its own
  Patch holds, `fillSamples`, with its frames, channels and rate) must be null, else it is refused (a heap block init
  made: a call would write what plays), and every parameter's stream is pointed at none. Both compile with the samples
  (Patch.h, "Samples"): `on: playing` with those the program that plays compiled with (`chatPlaying`), fresh with the
  plugin's as they are (`chatAssets`), as a RUN would. What plays is never written. `plugin.function` checks it; its
  notes are the debugging part's (read-only to MCP: `kFaces`).
- **write_file's files in and out** (`plugin/ChatImport.cpp`): `files` writes several as one change of the draft
  (`chatSetDraft`; a netlist without `*@` lines keeps its drawing, as one written alone), all or none; `from` brings in
  a file or a folder the user named, whole (`~/` the home folder): only the editor's kinds (`isProgramFile`: .fm, .cir,
  .gui, .sc, .sh), each text (no NUL, UTF-8), `kMaxImportBytes` (256 KB) at most and a name `validFileName` takes,
  else nothing comes in; a
  folder's hidden files, its other files and FolderSync's read-only copies (`.fmsync`'s copy lines, and a host.fm or a
  library file of this build's text) are left out and named; CR LF and what the editor does not hold become what
  `TextEditor::sanitise` makes them, each said (CR LF read as LF; the characters made '?' counted); added, or replacing
  the editor's file of that name, the program staying the first; never marked seen (Claude has not read them: the next
  state sends them), and the user's files only read. `to` writes the editor's files as they are, each through a
  temporary moved into place, to a new folder of the name given (a name, not a path) in `exportsFolder()`, the user's
  Music folder's `<Company>/<Name>/Programs` (the probe's sandbox's `programs/` under CTest), a number after a name
  taken: an export adds a folder, never replacing or deleting a file (two names equal but for case, one file on the
  default APFS, refused before anything is made; a file there already fails it; a failed one removes only the folder it
  made). It writes the program's samples there too, each file as it was added (`Asset::bytes`), their names in the
  case check; a project's source export carries them as well. `path` adds a sample (`ChatHost::chatAddAsset`, decoded
  on the calling thread; `name` its name if given): it is a form of its own, as `files`, `from` and `to` are, so
  `content` beside it is refused; an audio file's name (`assets::isAudioName`) is never a program file's, in `name`,
  in `files` or brought in by `from` (a folder's is left out and named, for `path` to add; one named alone refused).
  `plugin.import` checks them.
- **add_library_file** puts a library file in the editor as a tab (never over a file of that name) and returns its
  whole text, so Claude has seen it (`seen_`); the program's `#include` is Claude's to write.
- **projects** (`plugin/ChatProjects.cpp`, `plugin/ProcessorSaves.cpp`, `ChatHost::chatProjects`; ChatAgent.h's
  `ProjectCall`): Claude lists, saves, opens and applies the user's projects and presets, one tool with an `action`
  (list, save_version, save_preset, apply_preset, open: its properties undescribed, what each takes in the
  description's second paragraph, which the guide's projects part has, the tool budget's way). **Safe by design**, and
  keep it so: a save only adds (a version through `saveVersionAs`, a preset through `savePresetAs`, a factory project's
  save the user's own copy, `saveCopy`); a preset of the name is replaced only with `replace`, the result giving its
  old values; open and apply_preset are refused while the editor holds unsaved changes (`ProjectState::changed`: its
  files or samples; a dial off the version's or the preset's value, `baseDials_`, or off its default with no project
  open), naming them (`unsavedWhat`), unless `discard`, which the guide says only the user may ask for; a save, a
  preset and apply_preset are refused while an A/B is on or its Keep or Close still lands (`ab_.st.on`, `abEnding_`:
  what plays is a side's, not the editor's); nothing deletes (no action does: deleting is the user's, in PROJECTS).
  The saves, opens and presets run on the message thread, as PROJECTS runs them: the caller's worker hands poll() a
  job (`onMessageThread`, `runClaudeJob`: after the dials Claude set, so a save keeps a set_dial of the call before;
  at once when the caller is the message thread; a job not taken in `kClaudeJobSeconds`, or by the chat's stop (the
  message thread may be the one joining the turn: "The chats" above), withdrawn, nothing done:
  `setClaudeJobWaitForTests`, `claudeJobWaiting` for the probes) and waits in slices; an open then waits for its
  compile and for a poll() that set its dials (not past a stop either), so the next tool measures the version. An open
  of the plugin's own chat carries the conversation to the project opened (above, "The chats"). A list reads the store
  on the caller's thread (never a file whole on the message thread), the project open's state from a job. A version's
  note is Claude's ("Claude: <note> (<the change>)": `projectstore::saveVersion`'s `lead`, `noteLead`); its `category`
  is set with nothing new to save too.
  Each save, open and preset goes to the editor (`EditorPort::claudeNotice`, a count and the words, in capitals as
  every status notice, which the status line says in its sentence case: the Panel of the editor says it once in its
  status, polls the projects again and has an open PROJECTS read its lists again, `ProjectBrowser::reread`, which keeps
  the version and the preset chosen by number and name and withdraws a confirmation it was asking for (Delete, Open
  anyway, Replace, Add version), so no click confirms over lists that changed; a Panel made later says nothing of the
  past) and, the plugin's own chat's, to the journal as `Journal::Who::chat` (Claude Desktop's not: it knows); the
  chat's row says it. projects is none of `kChangingTools`: what it opens or applies is the user's asking, never
  Claude's work for the checks to send on; an open of another project clears the checks (`ProjectAnswer::another`,
  `ChatAgent::projectsText`), as a fresh start does, and its result says so. The guide says when to save (when the user
  asks, or at a milestone they agreed to; never silently) in RANDOM AND PROJECTS, and start names that part.
  `plugin.saves` checks both paths, `gui.presets` CLAUDE the editor's side.

## Recipes

- **An example**: a file `Source/fminus/examples/<key>/<n>-<name>.fm` whose first line is `// <Title>: what it is`;
  it appears in that product's EXAMPLES menu in file-name order (the first, `0-init.fm`, is the default program: a
  new one goes after it, or `fminus.init` and `plugin.process` fail). Run its product's
  `fminus.examples` (`scripts/test.sh <key> -R fminus.examples`). Write it in the house style: the editor's Format
  (Shift-Alt-F) changes nothing in it, which `fminus.lang` checks for every example and library file. An example may
  `#include` library files (the circuit examples do): they come with it as tabs (EXAMPLES starts afresh from one:
  `EditorPort::startAfresh`, as RANDOM does; `gui.dump`'s and `gui.explorer`'s `examples.*` rows), and whatever plays
  an example takes its files from `withLibraryIncludes` (`fminus/LibraryIncludes.h`), never `singleFile` alone.
- **A library file**: `Source/fminus/library/<n>-<category>/<name>.fm`, its first line `// <name>.fm: what it is`
  (the LIBRARY menu shows it, in both products), then `//` and a usage header (the comment lines it starts with, which
  Claude's instructions quote, and the editor's hover cards: a member's line, `filter.lp`, `y = filter.tick(x);`,
  then what it does after a gap of three spaces or more); in the house style (`fminus.lang`); names that no other library file uses; no allocation outside methods init and end
  call; safe before its set or allocate. Measure it in `fminus.library` (a harness there runs its modules over
  buffers), and run `plugin.chatexample` (the headers are in the prompt), for both products. A circuit netlist:
  `<name>.cir` in `6-circuits/`, its first line `* <name>.cir: what it is`, then `*` and the usage header in `*` lines;
  a module name no other library file uses; its configuration pinned to what `fminus <name>.cir --self-test` chose
  (that line in its header), so it never changes as it plays; `fminus.library` generates it, `fminus.circuit` plays
  it in both products.
- **A new call into programs** (a note's own pressure for the synth, or one when the host's transport starts, say;
  what the host or the player is doing belongs in the Host block instead): add a `Call` row in `Patch.cpp` (the
  method, whether it is required, how the plugin declares it, the appended line), a pointer in `Patch::Compiled`
  looked up in `compile()`, a method on `Patch` the Engine calls; document it in Patch.h and the README.
- **A view**: add an `Item` in `Source/editor/Panel.cpp` (a FunkGui widget, or your own drawing on the
  `funkgui::Canvas`, under a tag of its own after `ui::kMarkTag` when a probe needs to find it) and place it in the
  constructor; then check `gui.dump`'s PNGs, both products'. The editor draws only what changed (Panel.h, "Unchanged
  frames": `Panel::needsDraw`): an item's motion is `settled()` false while it moves; what its `draw()` reads that can
  change with no input, no motion and no `dirty_` of the Panel's (a port's value, a field, a callback's label, a
  countdown that ends) goes into its `drawKey()`; a callback the host runs later (a menu's, a file chooser's) goes
  through the Panel's host (`HostProxy`), which marks the Panel. A source that does neither freezes on screen until
  something else draws (at most a second: the safety net); `gui.dump` DRAW has a row for each source. A value that
  moves on its own and moves smoothly (a host's automation, a field's mark, a shader's input) is motion too, or it
  steps at the calm 12 Hz: `Items.h`'s `Stirred` in the item's `tick()`, held a quarter second past the last move.
  Input holds the full rate too, `kInputHold` (0.3 s) past the last event of any kind (`Panel::noteInput`: the pointer's
  move, press, drag, release and exit, the wheel, keys, a double-click, a file drag; a release and an exit also nudge the
  host's pump, which FunkGui's EditorHost does for the others), as SoftEditorHost holds it: a hover changes what is
  drawn at once, so without it the pump fell back to 12 Hz after each event's frame and the next waited (gui.switch
  RATE). A probe that checks the rate right after input ticks past the hold first.
  What changes in steps a few times a second (a readout's text) is a draw key, not motion: the hold would keep the full
  rate between steps. Only a shown item keeps the full rate (`wantsFullRate`), and a display settles when what it draws
  stands still (`DisplayItem`: a scope within half a px); gui.styles RATE and gui.panel SETTLE check, at the frame
  pump's cadence (`Tools/probes/common/Cadence.h`).
- **A .gui element** (a control, a display, a container): its `GuiKind` in `PanelSpec.h` and its word, arguments,
  properties and notes in `GuiSpec.cpp`, its natural size in `GuiLayout.cpp`, its item and drawing in `GuiItems` (a
  display's in `DisplayItem`), placed by `Panel::layoutGuiItems`; teach it in the prompt's THE INTERFACE
  (`ChatPrompt.cpp`) and GuiSpec.h's header; run `fminus.gui`, `gui.panel` and `gui.styles` (look at their PNGs) and
  `plugin.chatexample`. Shaders (`fminus/Shaders.h`: their string literals made glyph codes and their `#include`s
  resolved before shaderc (`shaderText`, `shaderIncludes`: `fminus/ShaderLib.h`'s library and the program's `.sh` files,
  each written beside the shader in an include guard; every included file's name and text in the cache's key, so a
  `.sh` changed builds again only the shaders that include it), built with the bundled shaderc and checked by the renderer,
  `ShaderTools::check`, FunkGui's `shaders::check`, so the message thread's `shaders::set()` runs no Metal compile; a
  panel's at once (`ShaderCache::buildAll`: a thread each but the first, shaderc `kShadercAtOnce` at most in a
  process), from a thread the compile starts before FMinus compiles (`readGuiShaders`: the inputs as written; a wrong
  guess costs a build, never the result), then for what the program has on the thread that compiles (the compiler's
  for a RUN, compileNow's caller for a host's state load), which waits for them; kept by `shaderCache()`, the
  process's, and on disk (`ShaderTools::cacheDir`, `shaderCacheFolder()`: the user's caches, the probe's sandbox under
  CTest), keyed by everything a build's bytes come from, shaderc's path, size and time among them, written under a
  temporary name and renamed into place, read back only whole, the folder's size kept as a running total and the
  folder scanned only now and then (`shaderCacheScans`); `GuiShaderItem`, registered by `GuiView::registerShaders`)
  have `gui.shaders` (CACHE, TIME); the build copies shaderc into each bundle (`CMakeLists.txt`) and CTest gives the
  probes its path (`<prefix>SHADERC`). A property every element of a kind could take goes through `class` and
  `defaults` without more: they hand on properties, not kinds.
- **A function of the shaders' library** (`funk/text.sh`, `funk/shapes.sh`): in `fminus/ShaderLib.cpp`, by the rules at
  its top (shaderc's HLSL front end on every renderer: Metal and Vulkan's SPIR-V compile here, Direct3D 11's needs
  Windows' d3dcompiler); it never reads the clock (`u_time`), or every shader that includes it would draw every frame. A
  change of the font's grid or glyphs moves `ShaderLib.h`'s numbers, which `gui.shaders` TEXT holds to FunkGui's bake.
  Teach it in the prompt's THE INTERFACE (`ChatPrompt.cpp`), then run `gui.shaders` (look at `gui.shaders.text.png`)
  and `plugin.sandbox`.
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
  `P.ge`, `P.in`, `P.eq`); helpers are in `Tools/probes/common/Audio.h`, `Tools/probes/common/Rig.h` and
  `Tools/probes/common/Cadence.h` (an editor's frames at FramePump's cadence, 60 a second or 12 when calm: a frame
  rate's row takes it rather than a cadence of its own). A row that bounds how long work takes times it with
  `probe::threadMs()` (the thread's CPU time: a busy machine's waits for a core are not counted) or against a
  calibration on the same thread, never the wall clock; a row whose condition a stalled thread can defeat (a wait
  that rightly takes a 20 ms gap for an idle audio thread) tells that try apart and makes it again (`plugin.reload`
  WAIT), so the suite passes under load without hiding a real slowdown.
- **The logo** (the mark, the app and extension icons, the README's header, the top bar's mark): change
  `Tools/logo/logo.swift` (or the instrument's accent in `Theme.h`), run `swift Tools/logo/logo.swift`, and look at
  `Assets/logo/sheet.png`, `construction.png` and the sizes you changed; then build (the bundles take the new
  `icon.icns` at configure) and run both products' `gui.dump` (MARK: the top bar's mark at 2x and at 1x, hinted for
  each, in its numbers and its pixels, and the 2x drawing at a zoom's other scales; `gui.dump.png`, `gui.dump.1x.png`).
  The editor draws `LogoMark.h`'s numbers as they are (Panel.cpp, by `Canvas::dpi()`), so the script is the only place
  a number changes; its mirror of FunkGui's rasteriser (the sheet's tiles) must follow FunkGui's `SoftRaster.cpp`: the
  sheet's tiles and `gui.dump`'s are the same pixels. A new extension icon raises `FP_MCP_VERSION`.
  A product renamed (`plugin.py rename`) runs it again: the lockups and the README's header carry the names.
- **A product**: `scripts/plugin.py add --from <key> --key <new> --name <Name> --code <Code>` writes
  `products/<new>.cmake` and copies the source product's examples and icon (same kind); CMake builds it with the
  others. Describe it in README.md and here.

## Git

Work on a branch; commit messages say what changed and why. Never commit build directories (`build-*/` is ignored).
Text about this repository's own history goes in `<!-- origin -->` blocks (README.md, CLAUDE.md), apart from what the
docs say about the code, so that a project made from this one can leave it out.
