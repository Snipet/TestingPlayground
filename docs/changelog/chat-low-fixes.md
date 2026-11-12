### Chat fixes: small things in the turn and the other providers

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
