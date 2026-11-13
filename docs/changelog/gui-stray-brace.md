### A stray { in a .gui file

**Fixed**
- A `.gui` file with a `{` where an element's name should be (a file that is only `{`, or a `{` alone inside a `row`)
  no longer hangs the compile while its notes fill the memory: such a `{` is read as if its braces were not there, so
  what it holds stays where it stands, with one note at it.
- A `.gui` file however broken says at most a page of notes, then one line that the rest were left out; and
  containers nested far past the limit no longer spend the stack on what is left out.
