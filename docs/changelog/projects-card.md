### The Projects card sorts four ways, exports several at once and closes on a click outside

**Added**
- The Projects card sorts by **Last modified**, **Date created**, **Size** or **Name**, each within its group, and
  **Reversed** turns any of them around. Each row shows what the list is sorted by: the date it was saved or made,
  or its file's size. The order is kept, the same in every instance; the explorer's list stays newest first.
- Choose several projects (Cmd-click, Shift-click) and **Export...**, or the context menu's **Export N projects...**,
  writes them all as one .zip, each project's own export inside it. The card shows how far it has come and Cancel
  stops it. **Import...** takes such a file and brings every project in it, and the status line says how many came
  in and which were refused, by name.

**Changed**
- A click outside the Projects card or the Export card closes it, as Close and Done do, and the click goes no
  further.
- An export never replaces a file. The save dialog suggests a name no file has; a file that is there already is
  refused, even when the dialog's own "Replace?" was answered yes, and the card asks for another name.
- Exporting one project no longer holds the editor while a large project is written: every export now runs in the
  background.
