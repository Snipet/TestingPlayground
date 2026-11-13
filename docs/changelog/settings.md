### Settings in every host, with pages and a Debug page

**Added**
- A **Settings** gear in the top bar, left of the theme's button, in every host as well as the Standalone. It opens a
  card of pages with their names in a rail at its left: **General** (the theme and the zoom, the version), **Editor**
  (Format on Run), **Projects** (the projects folder, with Show in Finder, Explorer or the file manager), **AI** (the
  chat's provider, model and key in words, and a button to the key dialog), **Debug** and **About**. The buttons and
  pills that set these already stay, and the card always agrees with them.
- The **Debug** page shows what this build is and what it is doing: the versions (this build's commit, FMinus's, LLVM's,
  FunkGui's, JUCE's, the Claude Desktop extension's), the graphics (the renderer, frames a second, the surface, the
  shader target), the program (its compile, its dials), the audio (rate, blocks, load, the synth's voices), the
  shaders, the folders the plugin keeps things in, Claude Desktop's link and the `TSTN_…`/`TSYN_…` switches set. **Copy
  all** puts it on the clipboard as plain text for a bug report. It shows no key, no word of a chat, no file's text and
  no audio, and it costs nothing while the page is closed.

**Changed**
- In the Standalone, Settings… (Cmd-, on macOS) and the effect's muted-input notice open the same card at its **Audio
  and MIDI** page, which holds the devices as before.
