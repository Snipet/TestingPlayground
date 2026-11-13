### The plugin's own failures no longer take the host down

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
