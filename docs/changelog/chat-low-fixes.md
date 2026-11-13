Claude Desktop extension: **0.20.0** (was 0.19.0).

### Chat fixes: the audit's smaller findings

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
