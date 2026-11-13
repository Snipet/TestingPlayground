### Find in all files

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
