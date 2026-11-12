Claude Desktop extension: **0.19.0** (was 0.18.0).

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
