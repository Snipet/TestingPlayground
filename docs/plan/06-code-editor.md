# 06. The code editor: find in files, the AI's grep, tabs, text size

Planning-document items: "Add find-and-replace", "Improve file tab UI", "Add support for resizing the text".
Size L, in four parts that are four pull requests. Decisions D4 and D9. Needs nothing (part C uses 01's `Scroll`
when it is there).

## 1. What exists

**Find and replace** (`Source/editor/CodeAssist.h/.cpp`): Cmd-F opens a bar at the text's top right; Cmd-Alt-F, or
the bar's chevron, adds the replace row. Match case ("Aa"), whole word ("ab"), next and previous (Return,
Shift-Return, Cmd-G), Replace and All (Alt-Return), one undo step for All (`TextEditor::applyEdits`). Matches are
found line by line in the shown tab only (`updateMatches`), at most 10,000, the fields 200 characters. No regex,
no other tab. A read-only tab (the IR view) never gets the replace row. Switching a netlist to its Schematic view
closes the bar. There is no button for any of it: only the keys, and a line in the README.

**The AI's tools** (`Source/plugin/ChatAgent.cpp`, `toolDefinitions`; `ChatFiles.cpp`): `read_file` with `find`
gives the lines that hold a text, two lines around each, 40 lines shown at most (`kMaxFound`), in one file or with
no `name` in every file; `regex: true` makes it a pattern (`dsp/Regex.h`: linear time, no captures by design, 20
million steps a call). `edit_file` replaces passages that each occur exactly once (`old_text`/`new_text`, `edits`,
or `files` as a list or a unified diff), all or none, one undo step, written only while the file is still what it
read (`chatSetDraftIf`). There is no replace-every-match. The tools/list reply is 23,400 bytes of a 24,000-byte
budget for the synth.

**The tabs** (`FilesItem`, `Source/editor/Panel.cpp`, constants `kTabH = 26`, `kTabMinW = 64`, `kTabMaxW = 150`,
`kTabNarrowW = 26`): each tab is a rounded pill (`rrect`, radius 8), the active one filled and outlined, so the
row reads as buttons. With many files the widest shrink first, down to 26 px showing the name's first letters
(`widthCap`, `narrow`); the bar never scrolls (`wheel` over it returns false). At the right: `+`, a netlist's
Netlist / Schematic pills, the IR tab. No context menu, no reordering, no key that changes tab. A program holds
16 files at most (`kMaxFiles`, `Source/dsp/Sources.h`); a tab closed is a file removed (the Panel asks first).

**The text size** is fixed: `funkgui::editorFont(code)`, 12 px, row height `round(px * 1.4)` (`makeTab` in
`Panel.cpp`, `irview::configure`). FunkGui can change it while running (`TextEditor::setStyle`: it rebuilds its
metrics and lays out again, keeps the text, the caret, the undo history and each line's colours, clamps the scroll
in px). The whole editor has a zoom of its own (100 to 200 %, `uiZoom`, the status bar's pill). The completion
list, the hover card, the signature and the find bar use fixed sizes (`CodeAssist.cpp`: `kRowH = 22`, `kCode`
12 px, `kCardCode` 11.5 px); only their placement follows the editor's row height. Cmd-=, Cmd-- and Cmd-0 are free
in a text tab (the Schematic view uses them for its own zoom).

## 2. Part A: find made findable, and across all files

**Findable:**
- A magnifier button at the left of the code's status line (`CodeAssist::StatusParts` gains `find`), with the
  title "Find and replace" and the three keys; a click opens the bar with the replace row.
- Ctrl-H on Windows and Linux opens find and replace (their convention; on a Mac Cmd-H is the system's Hide and
  stays so).
- The Standalone's Edit menu gains Find..., Find and Replace..., Find in All Files... (`plugin/StandaloneMenu`),
  with no shortcut of the menu's own (CLAUDE.md: a key the editor uses never becomes a menu shortcut).
- README, "The editor": the keys in the list of the code editor's keys, not only in a paragraph.

**All files:**
- A third toggle in the bar, "All files", beside Aa and ab. Cmd-Shift-F opens the bar with it on (free today).
- With it on, matches are found in every file tab: FMinus, `.gui`, `.sc`, `.sh` and netlists as text. Never the
  IR tab. In a netlist, never a `*@` line (the schematic's drawing: replacing in one would move the drawing).
- The counter reads "12 of 37 in 5 files". Next and previous walk every match in the tabs' order, changing the
  shown tab; a match in a netlist shown as a schematic switches that tab to its Netlist view.
- Under the bar a results list drops, eight rows shown, scrolling as the code does: `pedal.cir:42  R3 in out 10k`
  with the match marked; a click or Return goes there; Up and Down move through it.
- **Replace all** replaces in every file: one `applyEdits` a tab, tied into one undo step across the tabs by
  `FilesItem`'s undo groups (the mechanism a merge of several files uses: `UndoGroup`, `groupAt`, `groupStep`), so
  one Cmd-Z in any of them undoes them all. The notice: "REPLACED 37 IN 5 FILES". A read-only tab is never
  written.
- The caps hold across files: 10,000 matches in all; past it the counter says "10,000+".

Tests: `gui.code` FIND gains BUTTON, CTRLH (Windows and Linux rows), FILES (matches across three tabs and a
netlist; the IR tab and `*@` lines left out; the walk changes tab; the list's rows and click), REPLACEALL (every
file written, one undo in any tab undoes all, redo), SCHEMATIC (a result in a netlist shown as a schematic).
`gui.window` MENUS: the three entries. PNG `gui.code.find_files*.png`, both themes.

## 3. Part B: the AI's grep options and replace-all

All inside the two tools that exist (the tool budget: no new tool).

**`read_file`** gains three properties, each a few words:
- `context` (integer, 0 to 10; default 2): the lines shown around each hit.
- `count` (boolean): only how many hits each file has, no lines.
- `skip` (integer): pass over that many hits, to read on past the 40 shown. The result's last line says what is
  left: "37 more: call again with skip=40".

**`edit_file`** gains `find` (and takes `regex` as `read_file` does): every match of `find` is replaced by
`new_text`, in `name` or with no `name` in every file.
- The match is `read_file`'s: a text, or with `regex` a pattern, one line at a time, in find's own case.
  `new_text` is taken as it is: **no `$1`** (the engine keeps no captures).
- Never the IR view (it is no file) and never a netlist's `*@` lines.
- All or none, in one `chatSetDraftIf` of every file it read: one change of the draft, one undo step in the
  editor, a journal entry a file. `expected` holds for a named file. A change of the user's that came between is
  never overwritten: the replacement runs once more on the new text, as `edit_file` does now.
- Refused in words: no match (with the nearest lines, as a passage that does not occur gets); `find` beside
  `old_text`, `edits` or `files`; more than 2,000 replacements; a pattern past its step budget.
- The result: each file's count and new version, the first 20 changed lines numbered, and "seen" by the rule
  `edit_file` has (`carrySeen`).

Then: `kFaces` unchanged (both tools exist); the guide's THE TOOLS: WHAT THEIR SHORT DESCRIPTIONS LEAVE OUT gets
the details, the prompt's How you work one line ("to change a name everywhere, edit_file with find");
`FP_MCP_VERSION` raised. `regex`'s description in `edit_file` is `read_file`'s word for word, so MCP's copy drops
it (`dropRepeated`). Measure the reply: `plugin.mcp` prints each product's size and slack; if it passes 24,000
bytes, shorten another first sentence in the same change (CLAUDE.md, "The tool budget").

Tests: `plugin.chat` gains GREP (context 0 and 10, count, skip and its last line, across files) and REPLACE (one
file and all; regex; `*@` lines untouched; all or none when one file's `expected` fails; a user's change between
read and write kept; the refusals; one undo step: `gui.code`'s FILES row drives it through a Panel). `plugin.mcp`:
the budget row, and the tools' schemas through Gemini's subset (`plugin.providers` has the rows for schemas).

## 4. Part C: tabs that look like tabs, and scroll

**The look.** A tab strip joined to the editor below it:
- The active tab has the editor's own background, square bottom corners and rounded top ones (`Canvas::rrect4`),
  and no line under it, so it reads as part of the editor's card; a hairline runs under the rest of the strip.
- An inactive tab is its name on the strip's ground, a thin divider between neighbours, a tint on hover.
- The close cross shows on the active tab and on hover; the red disc for a file with errors stays where it is.
- A dot before the name when the tab's text differs from what plays (`cir::sameProgram` for a netlist, the text
  for the rest): today only the status says "edited", for all files at once.
- The `+`, the Netlist / Schematic pills and the IR tab keep their places at the right and never scroll; the IR
  tab keeps its outlined, apart look (it is a view, not a file).

**Many files.** Tabs keep their natural width (64 to 150 px) instead of shrinking to 26 px. When they do not fit:
- the strip scrolls sideways: the wheel over it (either axis), and a trackpad's sideways scroll, by `ui::Scroll`;
- the tab that becomes active is brought into view;
- a thin bar under the strip shows while the pointer is over it or it scrolls, and drags;
- the edge that hides tabs fades, and a chevron there steps one tab;
- a "files" dropdown at the strip's right (a `dropdown::` trigger: the chevron alone) lists every file, the
  shown one checked, for a jump with one click or a few typed letters.
- Ctrl-Tab and Ctrl-Shift-Tab go to the next and previous tab, files only, wrapping.

**Unchanged:** 16 files (D9); double-click renames; a tab closed asks first; no reordering (the first file is the
program, and an order means something to `#include` and to RANDOM: leave it).

Work: `FilesItem`'s `tabRect`, `tabWidth`, `widthCap`, `narrow` and the drawing (`Panel.cpp`, about lines 1340 to
1550 and 2396 to 2430) are replaced by a small `TabStrip` (in `Items.h`/`.cpp` or a file of its own) that owns
the scroll offset, the rects and the hit-testing; `FilesItem::drawKey` mixes its key; `setGeometry` gives it the
bar. The code window holds the same `FilesItem` (`lendEditor`), so nothing else moves. The probe hooks
`fileTabRect` and `centreOfTab` answer from it (a tab scrolled out has an empty rect, and `centreOfTab` scrolls it
into view first, as a user would).

Tests: `gui.ir` TABS changes (its rows assume narrowing: `tabs.files_clear_of_ir` stays true by scrolling;
`tabs.ir_room` stays); `gui.code` gains TABS (the active tab's shape and join, the edited dot, sixteen files
scroll, the active one in view, the wheel, the bar's drag, the chevrons, the dropdown, Ctrl-Tab, rename and close
still work, the least window). `gui.codewindow` re-checked. PNGs `gui.code.tabs*.png`, `gui.ir.tabs*.png`, both
themes; look at them: this part is a look.

## 5. Part D: text size

- **One preference, this machine's:** UiPreferences `"codeTextSize"`, in tenths of a px (ints only there), 90 to
  240, default 120. Not in the session: a size is the reader's, as the zoom is.
- **Changing it:** Cmd-= and Cmd-+ larger, Cmd-- smaller, Cmd-0 back to 12 px, in a text tab or the IR view (the
  Schematic view keeps those keys for its zoom); Cmd-wheel (Ctrl-wheel off the Mac) over the text, a step a
  notch; a stepper on Settings' Editor page (package 07); the status line shows the size when it is not 12 ("14
  px") and a click resets it. Steps: 9, 10, 11, 12, 13, 14, 16, 18, 20, 24.
- **What follows it:** every file tab and the IR view (`TextEditor::setStyle` with the same face and line
  spacing 1.4); the completion list's rows and text, the hover card's and the signature's code, and the find
  bar's fields, each sized from the editor's px in place of today's constants (their widths scale with it, capped
  by the editor's width). The minimap follows by itself. The chat, the status line and the tabs do not
  (assumption A7): the zoom is for the whole editor.
- **Keeping the place:** `setStyle` clamps the scroll in px, so the Panel notes the first visible line and the
  caret before the change and scrolls that line back to the top after it (`visibleLineRange`, `setScroll`).
- **Every open editor follows:** the Panel reads the preference in its refresh when `UiPreferences::revision()`
  moved (as EditorHost follows the theme and the zoom) and sets the style on its tabs. The code window holds the
  same tabs, so it needs nothing of its own. A headless Panel (a picture) reads it too, so view_panel's window
  picture is the user's size.

Work: `makeTab` and `irview::configure` take the size; `CodeAssist`'s constants become functions of it;
`FilesItem::wheel` and `key` take the modified wheel and the three keys before the TextEditor; `Panel::refresh`
watches the revision; `FilesItem::drawKey` mixes the size.

Tests: `gui.code` gains SIZE (each key and the wheel step the size and stop at 9 and 24; the first visible line
and the caret are where they were; the completion list, hover and find bar scale; a second Panel follows; the
Schematic view's zoom keys still zoom the drawing; the IR view follows); `gui.codepanel` SIZES re-checked; PNGs at
9, 12 and 24 px in both themes.

## 6. Docs

README, "The editor": find in all files and its keys; the tabs; the text size and its keys; "The chat": what the
two tools gained. CHANGELOG: Added (find in files, text size, the AI's options), Changed (the tabs). CLAUDE.md:
"The code editor" gains the tab strip's rule (the IR tab and the pills never scroll; a feature that walks the
tabs still walks the files) and the text size's (one preference; overlays sized from it); the chat's `read_file`
and `edit_file` paragraph gains the new forms; the budget's history line gains this step's bytes.
