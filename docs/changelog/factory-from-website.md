### Factory projects come from the website

**Changed**
- The plugins no longer carry factory projects of their own: FACTORY in PROJECTS and in the explorer shows the set
  fetched from the website (after you say yes) and nothing else. The placeholders that came with earlier builds are
  gone.
- A session saved with a factory project open still loads and plays with no factory projects at all, and names the
  project again once they arrive (unless you changed the program meanwhile).

**Added**
- While FACTORY is empty it says why in one row: the factory projects are not on this computer yet (**Get them**),
  they are being fetched, they could not be fetched (**Try again**), what arrived could not be checked, or they are
  turned off on this computer (and how to turn them on). The row is never a project: nothing chooses, stars or
  exports it.
- Claude's projects tool says the same when it lists projects and there are no factory projects yet; it never
  fetches them.
