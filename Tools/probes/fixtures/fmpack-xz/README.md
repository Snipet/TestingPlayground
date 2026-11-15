# plugin.export's xz fixtures

Packages (`.fmpack`, `Source/plugin/ProjectExport.h`) whose body is an .xz stream, as macOS writes them, for
`plugin.export`'s XZ rows: every platform must read them back to the exact JSON beside them. Nothing here is a real
user's: the projects are made up by `make.py`.

Made by `python3 Tools/probes/fixtures/fmpack-xz/make.py`, with XZ Utils 5.4.5 (`xz --version`); the same `xz`
writes the same bytes again. Each package is the 20-byte header (`FMPK`, format 1, method 2 for xz, the JSON's size
and its CRC-32, little-endian) and then the JSON compressed by `xz --format=xz --stdout` with:

| File | `xz` options | The stream's check |
| --- | --- | --- |
| `<kind>.json` | (none: the made-up project "Xz Fixture" of that kind, `json.dumps(..., indent=2)` and a newline) | |
| `<kind>-crc64-6.fmpack` | `-6 --check=crc64`: xz's defaults, and as Apple's `COMPRESSION_LZMA` most likely writes | CRC64 (ID 4) |
| `<kind>-crc32.fmpack` | `-6 --check=crc32` | CRC32 (ID 1) |
| `<kind>-sha256.fmpack` | `-9e --check=sha256` | SHA-256 (ID 10) |
| `<kind>-none.fmpack` | `-1 --check=none` | none (ID 0) |
| `effect-x86.fmpack` | `--x86 --lzma2=preset=6 --check=crc64`: x86's BCJ filter before LZMA2, which the plugins' decoder (XZ Embedded, built without BCJ: `Source/xz/XzOptions.h`) does not have, so it is refused | CRC64 (ID 4) |

**Kept byte for byte.** The probe compares what each package decodes to with its `.json` exactly, and takes the
expected size from it, so these files must be the committed bytes on every checkout: the repository's
`.gitattributes` marks this folder's `.json` files `-text` (git never turns their LF line endings into Windows' CR LF,
as it otherwise may on a Windows checkout) and its `.fmpack` files binary.

`<kind>` is `effect` or `instrument` (a product's `PRODUCT_KIND`): a project names its kind, and the other kind's is
refused on import, so each product imports its own. Every `.fmpack` reads back with `tail -c +21 <file> | xz -d`.

The damaged packages (cut short, a byte changed in the stream's header, its body and its footer, a wrong size in the
package's header, a check ID xz's format reserves) are made from these by the probe itself, at run time.
