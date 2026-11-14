# Changelog

What changed in the two plugins, TestingPlayground (the effect) and TestingPlaygroundSynth (the synth), newest first:
one section for each day work was merged, and under it each pull request with what you can do since. Each release has
a heading of its own, `## Version 0.1.0 (2026-11-10)`, above the days it holds; what is under `## Unreleased` comes
with the next one. The plugins' What's new shows the version you have from here.

When a day changed the Claude Desktop extension, its section says the extension's new version. Open the new `.mcpb`
(or run the installer again) to update it: Claude Desktop replaces an extension only with a newer version.

Each pull request's entry waits in its own file in `docs/changelog/` until `scripts/changelog.py` folds it in here.

## Unreleased

Claude Desktop extension: **0.21.0** (was 0.17.0).

### Projects in git: when both sides changed ([#70](https://github.com/Snipet/TestingPlayground/pull/70))

**Added**
- **Pull** on a project whose remote and this computer both have new versions merges them when they changed different
  things (different files, or different lines), saving the merge as a new version to push. When both changed the same
  lines, the Git card offers three ways, and nothing is lost by any of them: **Keep mine** (their side saved as a
  version, yours again on top), **Take theirs** (their side the newest version, yours the one before) and
  **Compare...**, which shows yours against theirs in the Compare card before you choose.

### Projects in git: Connect, Push, Pull, Check and Add from Git ([#66](https://github.com/Snipet/TestingPlayground/pull/66))

**Added**
- **Git...** in the Projects card (and the project pill's right-click menu) keeps a project in a git repository of
  yours, through the git on your computer and its own sign-in (an SSH agent, a credential helper): the plugin asks for
  and keeps no password. **Connect** commits every version to an empty repository and pushes them; **Push** commits and
  pushes the versions saved since, never forcing; **Pull** brings what another machine pushed as a new version, and
  opens it when the project is open and unchanged; **Check** counts what there is to push and to pull; **Disconnect**
  removes the link and keeps the project and its folder.
- **Add from Git...** clones a repository and adds its newest commit as a new project, linked.
- A linked project has a branch mark after its name in the Projects card, the explorer and the top bar's pill, with
  arrows and counts for what there is to push and to pull.
- The Settings card's Projects page says which git runs (**Look for git**, **Choose git...**) and where the working
  folders are.

### What's new ([#69](https://github.com/Snipet/TestingPlayground/pull/69))

**Added**
- What's new: the first time an editor opens on a version you have not seen (after the website's offer is answered,
  never over another card), a card shows that version's part of the changelog, built into the plugin: nothing is
  fetched. Versions you skipped show each under its own heading, the newest first. It scrolls, and its links open in
  your browser, only pages of the plugins' repository or of testingplayground.seanfunk.xyz. It shows once a version on
  your computer, whichever product opens first; Settings, General, What's new shows it again.
- `<PREFIX>WHATSNEW=off` never shows What's new by itself.
- The changelog by release: each release has a heading of its own over the days it holds, starting with Version 0.1.0
  (2026-11-10).

**Changed**
- A tag is released only once the changelog has its version's heading and no entry waits to be folded in.

### The website, asked once: the factory projects and a note when a new version is out ([#67](https://github.com/Snipet/TestingPlayground/pull/67))

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

### The plugin's own failures no longer take the host down ([#65](https://github.com/Snipet/TestingPlayground/pull/65))

**Fixed**
- A shader file whose code nests very deeply (`vec2(` inside `vec2(` thousands of times) no longer crashes the host, and
  a very long shader file no longer takes minutes to build: the plugin reads a shader's text in one pass, whatever its
  depth.
- When the plugin itself fails while compiling a program (out of memory, say), RUN now fails with an error that says the
  plugin failed, not your program, and what plays keeps playing; before, the host could crash. The same holds for the
  check while typing, the frozen copy of the dials, the circuits' self-test and a panel's shaders.
- An unexpected failure inside the plugin while Claude works (a turn of the chat, a tool, a call from Claude Desktop),
  while a project is read, saved, exported or previewed, or while A/B listening matches the loudness, now ends that
  piece of work with a message instead of crashing the host; the chat, the projects and Claude Desktop's link carry on.
- Moving quickly through many projects in the Projects card, or closing and reopening it, while a panel's preview was
  still being prepared could crash the plugin; it no longer does.

### Settings: the Usage page ([#64](https://github.com/Snipet/TestingPlayground/pull/64))

**Added**
- Settings has a **Usage** page: what the chat's requests used and cost, for every product on this Mac. Choose a range
  (Last 7 days, Last 30 days, Last 12 months, All time) and **Tokens** or **Cost**; a bar a day (a month in the longer
  views) is stacked by model, the largest a colour each and the rest as Other. Point at a bar, click it, or use Left,
  Right, Home and End with the chart focused to see that day's models under the chart, each with its tokens and cost.
- A table gives a row a model (fast mode a row of its own), its provider, requests, tokens in, cached and out, its cost
  and its share, and a total. Costs marked ~ are estimates at list prices as of the price table's date, which the page
  gives; OpenRouter's are its own figures; a model with no known price shows its tokens alone. Your provider's invoice
  is the record.
- **Reset…** empties the usage record, after asking in the page. A reset that fails changes nothing and says so.

### The Projects card shows a preview of a project's panel ([#61](https://github.com/Snipet/TestingPlayground/pull/61))

**Added**
- Choosing a project in the Projects card shows a picture of its panel above its versions, without opening it or
  compiling anything; choosing a version shows that version's. The dials are at the version's values and the displays
  show sample values, since nothing plays. It works for every project there is, factory projects too, and for a
  program with no panel.gui, whose dials show in rows or in its sections as the Panel view shows them.

**Changed**
- The Projects card is a little taller where the window allows, to make room for the preview; in a short window the
  preview is left out and the lists keep their room.

### Chat fixes: the audit's smaller findings ([#60](https://github.com/Snipet/TestingPlayground/pull/60))

**Fixed**
- Stopping a turn while it waited to ask again after an answer broke off no longer leaves the broken-off words in the
  conversation Claude reads next, though the chat had taken them off the screen.
- Stopping a turn while the planning model wrote its plan, or while the list of models was read, now says "Stopped."
  and keeps the part of the plan you had read on its card, instead of ending silently and erasing it.
- Claude's questions (ask_user) count their limits in characters, as they say: a label or a question in Japanese,
  Chinese or another script is no longer refused while within them, and a long header is never cut inside a letter.
- When a check still fails, the plugin's first nudge to Claude now says it will ask once more, as it does.
- A reply from an OpenAI-compatible server (Ollama, LM Studio, OpenRouter, OpenAI) cut off at its length no longer
  claims a limit of 64K tokens no request named: the chat says the server's limit cut it (on a server of your own, the
  model's context length in its settings).
- A server on this Mac that listed no model yet (Ollama with nothing pulled) is asked again at the next message, so
  a model pulled meanwhile is found without reopening the plugin.
- Error messages cut short (a proxy's page, a redirect's address) never end inside a character.
- An address for a server on the local network is read as every system reads it: one written with a leading zero
  (`http://010.0.0.5`, which the system reads as 8.0.0.5 on the internet), in hex or a short form, or with a backslash,
  a `%` or a port that is no number, is no longer taken as local; the chat asks for the plain address.
- Removing an API key on a Linux desktop whose session has no keyring service no longer says the key could not be
  removed: its file is deleted and the chat says it is removed.
- A project Claude Desktop opens no longer clears the checks of the chat you were in: they stay with that project's
  chat, as when you open a project yourself. Opening a project yourself while Claude Desktop's open is still loading
  keeps your project's chat on screen, instead of switching to the one Claude Desktop opened.
- Changing the model, the thinking level or web search no longer rewrites the chat shown: it keeps its time and its
  place in the list, and the chat you last talked in stays the one a project opens with.
- Typing in one file while Claude writes, renames, deletes or adds another no longer loses what you typed.
- read_file shows a line longer than a part read may hold (a table on one line) cut where a character starts, with its
  length, instead of nothing and advice to read the same line again.
- Claude's save of a project that already holds 100 versions says which version is no longer kept, instead of saying
  every version is kept; the projects tool and the guide say a project keeps its newest 100 versions, and that applying
  a preset is refused over moved dials (your files stay). A factory project saved by its name with no project open is
  named in the reply.
- Stopping the chat (or Claude Desktop cancelling) while Claude looks at the sound (view_sound) stops its render at
  once, as it stops the other renders, instead of waiting for the whole picture.
- tune_dials on files not run yet, with a dial the program that plays has not, no longer says every dial was set: it
  names the dial it could not set, with its best value, and says to run the program and set it.
- trace's export never replaces a file another instance exports under the same name at the same moment (the later
  one takes the next number), and an export that fails part way names the files it wrote.
- A render setting of the wrong kind (a number written as text, `hold_seconds: "5"`, a dial's value as a word) is
  refused in words instead of being dropped silently, and so is a check's index given as text; compare refuses the
  settings it does not read (notes, hold_seconds, ...) instead of ignoring them; view_sound's close-up with only one
  of its two ends, or with its end before its start, is refused instead of showing the default close-up.
- run_function on a fresh instance sets every dial as it is, an instrument's Shared dials and a circuit's pots and
  switches too, as its words say.
- compare's, trace's and view_panel's "playing" render the circuit configuration the program plays with, not a newer
  self-test answer it has not taken yet.
- On Windows and Linux the chat, its notes and Claude's tools say "this computer" where they said "this Mac" (the
  providers "Ollama (this computer)" and "LM Studio (this computer)", the chats kept, the exports), a path's
  example is that system's, and the key dialog's footer says Ctrl-V.
- set_checks describes a check's own dials_from and snapshot, as its listing gives them back, and no longer asks for
  a label (a check without one is named by its metric).
- write_file's sample path says it replaces a sample of the same name.
- Claude Desktop cancelling a tuning (tune_dials) leaves the dials as they were even when another of its calls is
  cancelled right after (closing a conversation cancels them all).
- Samples Claude Desktop adds, renames or deletes itself no longer head its own next result as a change of the plugin's
  own chat.
- A plugin opened while another instance's host is stopped (in a debugger, or crashing) no longer hangs; two instances
  started at the same moment no longer take one number for Claude Desktop, and closing one never removes the other's.
- A program that stopped as it played is told to Claude Desktop as a change, no longer as "the output guard acted".
- get_status's changes since a token show typing after a run as a line after the run's, not folded into the edit
  before it; the plugin's own chat is told that its diffs are Claude Desktop's alone (read_file shows a file).
- When the plugin's chat and Claude Desktop both work on one file, an edit refused because the file changed says which
  Claude's call changed it, not "one of your own calls".
- Claude Desktop names an instance by its number only when that instance is open ("Synth 3" with Synth 3 closed no
  longer reaches a track named "Inst 3"), and a track or project named in another script (Cyrillic, Japanese) is
  found in any case and by its words.
- The panel sandbox's apply, cancelled while it looks for the instance, writes nothing; when the plugin refuses the
  panel, the answer quotes the refusal; when the program does not run, it says so; and the whole apply answers within
  one call's time.
- The panel sandbox refuses a file that is no regular file (a pipe, a device) in a folder it opens, instead of waiting
  on it until it gives up, and never reads more than its limit.
- The MCP server answers a batch of requests (a client of protocol 2025-03-26 may send them) and answers JSON that is
  no request as an invalid request.
- Copy under an answer or a code block copies the text as Claude wrote it (accents, other scripts, symbols such as °
  and µ), not the plain-letter form the chat draws.
- Your own message is shown as you typed it: asterisks and backticks (`200*2`, `float* out`) are no longer dropped.
- Inline code in small text (opened thinking, quotes, an option's description, a check's detail) no longer runs over
  the word after it.
- A row of what Claude did is red only when it failed (a run that did not compile or stopped is now red; writing a
  file named `errors.fm` is not). Chats kept before this show their rows plain.
- An opened thinking row, a folded plan or an open question's picks no longer carry over to another chat you open.
- An answered question marks only the option chosen, not also one whose name begins or ends it ("Yes" beside "Yes,
  but quieter").
- The conversation scrolls from the keyboard: Shift-Tab from the message box reaches it, then the arrows, Page Up and
  Down, Home and End scroll it. A screen reader reaches every message, and moving to one out of view scrolls it in.
- The send button sends a message of pictures alone, as Return does.
- A long server's name as the chat's title is cut to leave the chat list its button, never drawn over the header's
  buttons.
- Pictures that do not fit one row of the message box wrap onto a second, so all six are shown and can be removed.
- The key dialog works from the keyboard: Tab reaches the Provider pill, Remove key, Cancel and Save, and Return or
  Space presses the one with the focus (the Provider pill opens its menu). The wheel no longer scrolls the panel behind
  the dialog.
- Dragging an audio or project file over the editor no longer lights the chat's "Drop pictures here" overlay.
- A click in the key dialog's server address, model or a chat's new name places the caret there (and drops the
  selection); an address or model longer than its field scrolls to show what is typed; a long refusal wraps onto a
  second line instead of being cut.
- A refused Save in the key dialog is said by a screen reader, and the reason is on the field it is about.

**Changed**
- Claude Fable 5.2 is in the model list before your key's list arrives (in place of Fable 5.1), and Fable 5.2 and
  Mythos 5.2 offer **Fast** and are rerouted by the API's fallbacks when a classifier declines a request, as Opus 5.5
  is.

### The Projects card sorts four ways, exports several at once and closes on a click outside ([#59](https://github.com/Snipet/TestingPlayground/pull/59))

**Added**
- The Projects card sorts by **Last modified**, **Date created**, **Size** or **Name**, each within its group, and
  **Reversed** turns any of them around. Each row shows what the list is sorted by: the date it was saved or made, or
  its file's size. The order is kept, the same in every instance; the explorer's list stays newest first.
- Choose several projects (Cmd-click, Shift-click) and **Export...**, or the context menu's **Export N projects...**,
  writes them all as one .zip, each project's own export inside it, named "<plugin> Projects.zip" unless you name it.
  The card shows how far it has come and Cancel stops it. **Import...** takes such a file and brings every project in
  it, and the status line says how many came in and which were refused, by name.

**Changed**
- A click outside the Projects card or the Export card closes it, as Close and Done do, and the click goes no further.
- An export never replaces a file. The save dialog suggests a name no file has; a file that is there already is refused,
  even when the dialog's own "Replace?" was answered yes, and the card asks for another name.
- Exporting one project no longer holds the editor while a large project is written: every export now runs in the
  background.

### Faster Windows builds on every pull request ([#58](https://github.com/Snipet/TestingPlayground/pull/58))

**Changed**
- The builds on GitHub's Windows machines keep their compiled objects between runs too (sccache, as macOS and Linux
  keep theirs with ccache): a run whose code changed little rebuilds little. The log says how many objects came from
  the cache and how large it is.
- A build run by hand can build only some of the platforms (`platforms`: `windows-x64`, say), so a branch's Windows
  build can be tried without a macOS machine.

### The pointer drives a panel ([#54](https://github.com/Snipet/TestingPlayground/pull/54))

**Added**
- A panel can have controls of its own making. In a `.gui` file, an `area` (a new element that draws nothing), a
  `shader`, a row, column, grid, section, box, label or display can name parameters the pointer writes over it: `x=`
  and `y=` (where it is, across and up), `down=` (while it is pressed), `wheel=` and `hover=` (while it is over it: one
  of the panel's own dials). `drag=relative` moves by the pointer's travel instead, `release=spring` puts `x` and `y`
  back when let go, and `cursor=` shapes the pointer. One press is one gesture: one automation pass in the host, one
  undo step. A knob inside such a box still turns, and a display inside it passes the press on.
- A shader can read the pointer over it as inputs, `pointer.x`, `pointer.y`, `pointer.down` and `pointer.hover`, so a
  control can be drawn entirely by a shader and dragged like a knob; it draws again only when they change.
- Such an element is a Tab stop: the arrows move it, Home puts it back, Space presses it. A screen reader sees a
  slider for each of `x` and `y` and a button for `down`. A right-click opens the first parameter's menu, MIDI learn
  included.
- Two examples drawn this way: Light filter (the effect: a shader dial for the cutoff, and a spectrum to drag it
  across) and Light saw (the synth: a shader dial for the brightness, and a scope to hold down for a swell). An example
  can now be a folder, its program with its `panel.gui` and shader beside it.
- Claude is taught the pointer, with a shader dial as its example, and view_panel and the GUI sandbox draw a panel as
  if the pointer were over its shaders or pressing them (`values` naming `pointer.hover`, `pointer.down`, `pointer.x`,
  `pointer.y`).

**Changed**
- A shader's `width=` and `height=` are now its size. Before, a shader was never smaller than 200 x 120 px, whatever
  they said: a panel written with `shader "dash.sc" width=170 height=110` drew it 200 x 120, and now draws it 170 x 110,
  as written. To keep the old look, give it `width=200 height=120` (or leave both out: 200 x 120 as before).

### Find in all files ([#57](https://github.com/Snipet/TestingPlayground/pull/57))

**Added**
- Find in all files: Shift-Cmd-F (Shift-Ctrl-F on Windows and Linux), or the find bar's new All files toggle, finds in
  every file's tab at once, `.gui` files and netlists included; never in the IR view, and never in a netlist's `*@`
  lines, which hold its drawing. The counter says "12 of 37 in 5 files", the next and the one before walk every match,
  showing its tab (a netlist shown as a schematic switches to its Netlist view), and a list under the bar has a row a
  match: a click goes there, Up and Down walk it. All replaces in every file, and one Cmd-Z in any of them undoes it
  in all of them.
- A magnifier at the left of the code's status line opens find and replace. On Windows and Linux, Ctrl-H opens it too.
- The Standalone's Edit menu has Find..., Find and Replace... and Find in All Files....
- The README lists the find bar's keys.

### Settings in every host, with pages and a Debug page ([#49](https://github.com/Snipet/TestingPlayground/pull/49))

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

### Compare two versions of a project ([#48](https://github.com/Snipet/TestingPlayground/pull/48))

**Added**
- **Compare...** in the Projects card shows what changed between two versions of a project, or between a version and
  what is in the editor now: the files line by line, the dials and the samples, without playing or opening anything.
  Choose either side from its dropdown, swap them, step from change to change with **]** and **[**, then open a
  version or listen to both in A/B from the card. A version's right-click menu and the top bar's project pill's
  right-click menu ("Compare with the saved version", while the editor has unsaved changes) open it too.
- A netlist whose drawing alone changed shows as one row, "the drawing changed", in Compare and in the A/B card.

**Changed**
- The A/B card reads a project's version list without reading the whole project file on the panel's thread, so a
  large project no longer makes the panel stall as the card opens.

### A stray { in a .gui file ([#56](https://github.com/Snipet/TestingPlayground/pull/56))

**Fixed**
- A `.gui` file with a `{` where an element's name should be (a file that is only `{`, or a `{` alone inside a `row`)
  no longer hangs the compile while its notes fill the memory: such a `{` is read as if its braces were not there, so
  what it holds stays where it stands, with one note at it.
- A `.gui` file however broken says at most a page of notes, then one line that the rest were left out; and
  containers nested far past the limit no longer spend the stack on what is left out.

### The chat counts its tokens and says what a turn cost ([#50](https://github.com/Snipet/TestingPlayground/pull/50))

**Added**
- The line under the message box adds what the turn has cost so far, once every request in it has a price: "~$0.07"
  is an estimate at list prices (Anthropic's from a table built into the plugin, dated 2026-11-12; an OpenRouter
  model's from its models list), "$0.04" a provider's own figure (OpenRouter's). A model with no known price shows its
  tokens and no cost.
- Every request the chat sends is counted, by day and by the model that answered, in `usage.json` beside the chats:
  retries, the planner's and fast mode's and web search's second tries included, and the tokens written to the cache
  for five minutes and for an hour apart. It keeps counts and model names only, never a word of a chat, a key or a
  server's address. A page to see it comes later.

### Tabs that look like tabs, and scroll ([#52](https://github.com/Snipet/TestingPlayground/pull/52))

**Added**
- Ctrl-Tab and Ctrl-Shift-Tab go to the next and the previous file.

**Changed**
- The files' tabs read as tabs: the one shown has the editor's own background and opens into it, the others are their
  names with a thin line between them. A dot before a name says its text has changed since the program last ran.
- With many files the tabs keep their names whole instead of narrowing to a letter: the strip scrolls sideways (the
  wheel over it, or a trackpad), the tab shown stays in view, an arrow at an edge shows the next tab, and a menu at the
  strip's right lists every file.

### Chat fixes: keys and chats on a full disk, Send and Stop, typing against Claude, pictures, streaming ([#51](https://github.com/Snipet/TestingPlayground/pull/51))

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
- Text Claude wrote with characters the editor does not show (an arrow, an accent, a degree sign) went into the
  program as written while its tab showed `?`, and the next key you pressed anywhere changed Claude's file and counted
  as your edit, so Claude's next edit of it was refused. Claude's text now goes in as the editor holds it (printable
  ASCII, tabs and line ends), what plays and what you see are the same, and the tool's answer says what became `?`.
- On Windows and Linux a picture file of a few kilobytes whose header claimed a huge size could make the plugin use
  gigabytes of memory, or crash the host, when added to a message. A picture of more than 100 megapixels is now
  refused before it is read, saying its size; a photo or a screenshot is taken as before.
- A recording of a program that plays a sample said its replay differed from what played, when it matched: the
  check replayed it without the samples. It now says the replay matches bit for bit.
- A note or a dial's glide that ended past a render's limit (10 s for a picture, 300 s for numbers) was cut short
  without a word, and the answer described the times asked. It is now refused, naming the note or the move and the
  limit, as a start past the limit was; one that ends at the limit is taken as asked.
- Claude Desktop is told that recording what you play is no read-only look: arming it changes what plays (a restart
  starts the program again), and a new recording or forget lets the last one go, so Claude Desktop asks before it
  does it.
- Claude Desktop's panel sandbox, opened on a program's folder, left its circuit netlists out, so a circuit's knobs
  were missing; it put a file that sorts before `program.fm` in the program's place, and counted the folder sync's
  read-only copies against its sixteen files. It now reads a folder as the plugin does.
- On macOS and Windows Claude's answer (its words, its thinking, a file it writes) now appears as it is written,
  rather than all at once at the end or in jumps of a few paragraphs.

### The code's text size ([#46](https://github.com/Snipet/TestingPlayground/pull/46))

**Added**
- The code's text can be made larger or smaller: Cmd-= (or Cmd-+) and Cmd--, from 9 to 24 px, and Cmd-0 back to
  12 px; or Cmd and the wheel over the text (Ctrl on Windows and Linux). Every tab and the IR view follow, and every
  other editor open on the machine; the line at the top and the caret stay put. The completion list, the hover card,
  the signature and the find bar grow with the text. While the size is not 12 px the status line shows it, and a
  click there puts it back. It is a preference of the machine, as the zoom is, not of a session.

### Claude searches with more control and replaces every match ([#45](https://github.com/Snipet/TestingPlayground/pull/45))

**Added**
- Claude's search of the files (read_file's find) shows as many lines around each line found as it asks (none to
  ten), can give only how many lines each file has, and reads on past the first 40 (its last line says how many are
  left and how to read them).
- Claude can replace every match of a text or a pattern at once (edit_file's find), in one file or in all of them: a
  name changed everywhere is one step you undo with Cmd-Z. It replaces within a line and in the text's own case, never
  a netlist's drawing lines, at most 2,000 at once, all or none; it never overwrites what you typed meanwhile.

### Faster builds on every pull request ([#41](https://github.com/Snipet/TestingPlayground/pull/41))

**Changed**
- The builds on GitHub's machines keep bgfx's shader compiler between runs, on macOS, Linux and Windows, and on macOS
  and Linux the compiled objects too (ccache): a run whose code changed little rebuilds little. `main` keeps the
  caches fresh; a pull request saves its own only when it found none to start from. Each run's log says how many
  objects came from the cache.
- A pull request that changes only the docs (`docs/` and the `.md` files beside this one) builds and tests nothing:
  its checks pass at once.

### Every list scrolls as the code does

**Fixed**
- The explorer, the Projects card's lists, the presets' list and the A/B card scroll the way the code does with
  natural scrolling on. On a Mac they scrolled the other way, and the Settings card, a scrolling interface on the
  panel and a schematic's pan and probe lanes did too.
- Those lists, the A/B card's changes and the Settings card move as far as the code for the same turn of the wheel or
  stroke of the trackpad: three rows a notch, and a trackpad's stroke a px for a px. They scroll by the pixel, so a
  row at an edge shows in part. The Projects card and the presets' list used to jump a whole row for each trackpad
  event, and the explorer moved about four times slower than the code on a trackpad.

**Changed**
- Every scrolling list, card, the chat and a scrolling interface have a thumb you can drag, as the code has, and a
  click on its track jumps there. A notch of the wheel moves the chat three lines and an interface 72 px.

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

## Version 0.1.0 (2026-11-10)

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
