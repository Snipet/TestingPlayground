### The chat summarizes a long conversation when it has to, and only once

**Fixed**
- The chat no longer summarizes a long conversation far too early: it now waits until the conversation fills half of
  what the model can read (500,000 tokens on a model that reads a million, never sooner than 150,000), where it used to
  summarize at 150,000 on every model.
- The chat no longer goes round summarizing request after request when the program's files and what Claude is told
  about the plugin already fill most of the model's room, each summary a request you paid for. It gives the
  conversation more room where the model has it; where it has none, the turn ends with a note that says what fills the
  room, in numbers, and what helps: a model that reads more, smaller or fewer files, or a new chat.
- Right after a summary Claude has every file whole and your last message as you sent it, pictures included, so it
  carries on from where it was instead of reading the files again.
