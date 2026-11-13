### The website, asked once: the factory projects and a note when a new version is out

**Added**
- The first time an editor opens, a card asks **Use the website?**: the factory projects, kept up to date, and a note
  when a new version is out, both ticked. **Allow** says yes to what is ticked; **Not now** is remembered and the card
  is not shown again by itself. It stops nothing: the plugin plays and the editor works behind it.
- A pill in the top bar when a new version is out ("0.2.0 is out"), and the status line says it once. A click opens a
  card with the release's summary, **Download** (the website's download page, in the browser), **Skip this version**
  and **Later**. Nothing opens by itself.
- Settings has a **Network** page: **Check for updates** with what the last check found, **Check now**, and **Get the
  factory projects**, each saying what a request sends: the plugin's name and version, nothing else.
- `<PREFIX>UPDATES=off` never checks and never shows the card.

**Changed**
- Besides the chat, the plugin now reaches testingplayground.seanfunk.xyz, only after a yes and only once an editor has
  opened: never while a host scans its plugins or a session plays with no editor open.

**Fixed**
- On Windows, with both plugins open (or two instances in two hosts), Settings' Reset of the usage record could say it
  failed, and a count or an answer to the website's offer could go unwritten, when the other was reading the same file
  at that moment. The write now waits for the reader, half a second at most.
