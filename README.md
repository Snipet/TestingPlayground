# TestingPlayground

A starter JUCE audio plugin (AU / VST3 / Standalone) for Apple Silicon,
using [visage](https://github.com/VitalAudio/visage) for the GUI. Build
it as-is to confirm your toolchain works, then copy the directory and
rename to start a new plugin.

## What it does

- **Audio:** single `Gain` parameter (-24..+24 dB), smoothed per sample,
  exposed via APVTS so the host can automate it.
- **GUI:** a visage `ApplicationWindow` hosted inside the JUCE editor,
  rendering the basic example from the visage repo — a cyan circle on
  a navy background. Add controls and bind them to APVTS from there.

## Building

Requires:

- macOS 14+ on Apple Silicon (M1 or newer).
- Xcode 15+ command-line tools.
- CMake 3.22+.

```sh
cmake -B build -G Ninja
cmake --build build --config Release
```

`COPY_PLUGIN_AFTER_BUILD TRUE` deploys the AU and VST3 into your user
plugin folders; the Standalone runs from the build tree.

## Starting a new plugin from this template

Use the scaffolding script:

```sh
./scripts/plugin.py clone
```

It prompts for the new plugin name, plugin type (effect / instrument /
midi), company, manufacturer and plugin codes, bundle ID, DSP namespace,
and destination directory — most have sensible defaults derived from the
name. It then copies the template, performs case-sensitive string
replacement throughout, and reconfigures the bus layout / MIDI flags /
`IS_SYNTH` / `IS_MIDI_EFFECT` lines to match the chosen type.

The script is copied into every clone, so you can keep cloning from any
plugin you've already made.

Once cloned, replace the `Gain` DSP example under `Source/dsp/` with
your own and add the `.cpp` files to `target_sources(...)` in
`CMakeLists.txt`. Adding parameters is a four-step pattern visible in
the existing `gain` example.

## Other scripts/plugin.py subcommands

```sh
./scripts/plugin.py rename                          # rename current plugin in place
./scripts/plugin.py codesign --identity "Developer ID Application: ..."
./scripts/plugin.py notarize --keychain-profile "AC_PASSWORD"
```

`codesign` strips extended attributes (`xattr -cr`) from each bundle
first, then re-signs with a Developer ID Application identity (replacing
JUCE's ad-hoc signature). `notarize` zips each signed bundle, submits it
via `xcrun notarytool`, waits for the result, and staples the ticket. Set
up the keychain profile once with `xcrun notarytool store-credentials`.

> **Note:** After running `codesign`, the bundles in `~/Library/Audio/Plug-Ins/`
> gain immutable flags set by the system codesigning machinery. If you then
> rebuild, JUCE's copy step will fail with "Operation not permitted". Fix with:
> `chflags -R nouchg ~/Library/Audio/Plug-Ins/Components/<name>.component`
> (and the same for the `.vst3`), or just delete and let the build reinstall.

## Architecture

```
CMakeLists.txt              Build config + Apple Silicon toolchain pinning
scripts/
  plugin.py                 clone / rename / codesign / notarize utility
Source/
  PluginProcessor.{h,cpp}   AudioProcessor + APVTS + parameter dispatch
  PluginEditor.{h,cpp}      AudioProcessorEditor + visage host
  StandaloneApp.cpp         Custom JUCE Standalone window (native title bar,
                            no banner) — only compiled in Standalone target
  dsp/
    Gain.h                  Example DSP — replace with your own
```

Parameter flow: APVTS listener → `parameterChanged` in the processor →
typed setter on the DSP object → DSP smooths internally. The audio
thread only ever reads atomics from the DSP class, never touches APVTS
directly, and does no allocation. Preserve this split when you extend.
