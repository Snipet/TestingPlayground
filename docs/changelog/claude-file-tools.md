Claude Desktop extension: **0.18.0** (was 0.17.0).

### Claude searches with more control and replaces every match

**Added**
- Claude's search of the files (read_file's find) shows as many lines around each line found as it asks (none to
  ten), can give only how many lines each file has, and reads on past the first 40 (its last line says how many are
  left and how to read them).
- Claude can replace every match of a text or a pattern at once (edit_file's find), in one file or in all of them: a
  name changed everywhere is one step you undo with Cmd-Z. It replaces within a line and in the text's own case, never
  a netlist's drawing lines, at most 2,000 at once, all or none; it never overwrites what you typed meanwhile.
