### Packages made on a Mac open everywhere

**Fixed**
- A project exported as a package (`.fmpack`) on a Mac now opens on Windows and Linux, alone or inside a bundle of
  several projects: those packages are compressed with xz, which only the Mac version could read until now. A package
  that is damaged is refused with a reason, as before. Packages written on Windows and Linux are unchanged and still
  open everywhere.
