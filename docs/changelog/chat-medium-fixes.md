Claude Desktop extension: **0.19.0** (was 0.18.0).

### Chat fixes: keys and chats on a full disk, Send and Stop, typing against Claude, pictures, streaming

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
