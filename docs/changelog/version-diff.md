### Compare two versions of a project

**Added**
- **Compare...** in the Projects card shows what changed between two versions of a project, or between a version and
  what is in the editor now: the files line by line, the dials and the samples, without playing or opening anything.
  Choose either side from its dropdown, swap them, step from change to change with **]** and **[**, then open a
  version or listen to both in A/B from the card. A version's right-click menu and the top bar's project pill's
  right-click menu ("Compare with the saved version", while the editor has unsaved changes) open it too.
- A netlist whose drawing alone changed shows as one row, "the drawing changed", in Compare and in the A/B card.

**Changed**
- The A/B card reads a project's version list without reading the whole project file on the panel's thread, so a
  large project no longer makes the panel stall as the card opens.
