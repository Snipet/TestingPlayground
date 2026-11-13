### The plugin survives what its background work cannot do

**Fixed**
- A shader (a `.sc` or `.sh` file) with vector constructors nested tens of thousands deep no longer overflows the
  stack on a shader-building thread and ends the host: the shader is rewritten in one pass, in time that stays
  proportional to its length however it is nested or left unclosed.
- A failure the plugin's own background work cannot recover from — running out of memory, or any error inside the
  plugin — no longer ends the host. A compile that fails this way becomes a failed RUN whose message says the compile
  failed inside the plugin and why, and the program that was playing keeps playing; a shader that fails is a note on
  its line; a check while typing gives no messages; freezing, the circuit self-test, a save, an export, loading the
  factory projects, matching A/B loudness and reading a picture each report their own failure and carry on.
- A tool that fails this way in a chat with Claude, or in a call from Claude Desktop, now answers with an error in
  words and the conversation goes on; a chat turn that fails outside its tools ends with a line saying so, and the
  next message is taken as usual.
