# Factory fixtures

Test fixtures for the plugins' factory projects (the website repository's `docs/factory-projects.md`, sections 4
and 5): known answers for the signature verifier and SHA-256, and one little site per case with what a plugin must
do with it. They are made by the website repository's `scripts/factory-fixtures.ts`, with that repository's own
`scripts/factory.ts` (the formats and every check a plugin makes), so they are what a publish makes:

```bash
npm run factory-fixtures -- <the plugins' factory/ folder> <this folder>
```

Each run makes new keys and writes only their public halves, so every file with a signature or a key changes
when the fixtures are made again; the cases stay. No private key is kept anywhere. Before it finishes, the
script checks every case again from the files it wrote, with the same `checkSet` a publish runs.

## The files

- `keys.json`: the test public keys by id (`t1`, `t2`), each the curve's point (65 bytes: 04, x, y) in base64, in
  the shape of the website's `factory-keys.json`: what a plugin under test has built in.
- `vectors.json`: `p256`, a list of `{ name, point, message, signature, valid }` (base64) for ECDSA P-256 over
  SHA-256 of `message`, the signature r then s (64 bytes); `valid` is false for anything that is not a 65-byte
  point starting 04 and a 64-byte signature with r and s in [1, n - 1] that verifies. `valid-high-s` is valid:
  ECDSA allows s in the upper half of the order, and a verifier that enforces low s would wrongly refuse it.
  `valid-r-leading-zero` and `valid-s-leading-zero` have a scalar starting with a zero byte, which a DER encoder
  drops. `point-compressed` and `point-hybrid` are the right key in forms the format does not take. `sha256`, a list of
  `{ name, message, repeat, sha256 }`: the hash (hex) of `message` (base64) repeated `repeat` times.
- `cases.json`: one row per case and product: `{ case, key, expect, reason?, step?, highest?, projects? }`. `key`
  is the product (`effect`, `synth`); `expect` is `taken` or `refused`; `reason` is why, as `factory.ts` words it;
  `step` is which check refuses it (`size`, `signature`, `index`, `sequence`, `file`, `project`: section 5's
  order); `highest` is the highest sequence the plugin took before (0 when absent); `projects` the names a plugin
  of version 0.1.0 or later (below 99.0.0) takes, in the index's order. Every entry needs 0.1.0 unless the case says otherwise.
- `<case>/factory/v1/<key>/index.json` and `<case>/projects/v1/p/<sha256>.fmpack`: the site as the plugin would
  fetch it (the paths after the site's address).

## The cases

| Case | Product | A plugin | What it is |
| --- | --- | --- | --- |
| `good-1` | effect | takes 3 | Both products, sequence 1: the staged projects and two made-up ones (Fixture Changed, Fixture Withdrawn). |
| `good-1` | synth | takes 4 | Both products, sequence 1: the staged projects and two made-up ones (Fixture Changed, Fixture Withdrawn). |
| `good-2` | effect | takes 3 | Both products, sequence 2 over good-1: the staged projects unchanged (each file the same bytes and hash as in good-1), Fixture Changed changed (its program has a line more: a new file), Fixture Withdrawn withdrawn, Fixture Added added. After taking sequence 1. |
| `good-2` | synth | takes 4 | Both products, sequence 2 over good-1: the staged projects unchanged (each file the same bytes and hash as in good-1), Fixture Changed changed (its program has a line more: a new file), Fixture Withdrawn withdrawn, Fixture Added added. After taking sequence 1. |
| `good-1` | synth | refuses: "the index is older (1) than one taken before (2)" | Both products, sequence 1: the staged projects and two made-up ones (Fixture Changed, Fixture Withdrawn). After taking sequence 2. |
| `key-change` | effect | takes 3 | good-1's projects at sequence 3, signed with t2, the next key (the plugin has both). |
| `key-change` | synth | takes 4 | good-1's projects at sequence 3, signed with t2, the next key (the plugin has both). |
| `needs-newer` | synth | takes 2 | The synth's staged projects and Fixture Newer, whose entry needs 99.0.0: a plugin leaves that entry out and takes the rest. |
| `changed-body` | synth | refuses: "the signature does not verify" | The body changed after signing (its sequence 1 made 2). |
| `wrong-key` | synth | refuses: "the signature does not verify" | Names t1, signed with t2. |
| `unknown-key` | synth | refuses: "the index is signed with a key the plugins do not have (t9)" | Names t9 and is signed with it: a key not in keys.json (its public half is not written anywhere). |
| `signature-short` | synth | refuses: "the signature is not 64 bytes" | A signature of 63 bytes (the last byte cut). |
| `signature-der` | synth | refuses: "the signature is not 64 bytes" | The right signature, DER-encoded: the format is r then s, 64 bytes, so DER is refused by its length. |
| `other-kind` | synth | refuses: "the index is for kind effect, not synth's" | The effect's good set served where the synth's index is. |
| `other-plugin` | synth | refuses: "the index is for TestingPlayground, not TestingPlaygroundSynth" | The synth's kind, the effect's plugin name. |
| `duplicate-name` | synth | refuses: "the index lists PLACEHOLDER BASS twice" | A second project whose name is the first one's in another case (its own file, consistent): names are unique case aside. |
| `too-many` | synth | refuses: "the index lists no projects, or too many" | 257 entries, all naming one file: the count is refused before any file is read. |
| `index-too-large` | synth | refuses: "the index is larger than a plugin takes" | A good index, signed, of 262145 bytes (its body carries a long field no check reads): refused by its size alone, before it is parsed. |
| `no-needs` | synth | refuses: "Placeholder Bass: it does not say which plugin version it needs" | The first entry has no needs. |
| `file-hash` | synth | refuses: "Placeholder Bass: its file does not match its SHA-256" | The first entry's file with its last byte changed (the same size). |
| `file-size` | synth | refuses: "Placeholder Bass: its file is 1260 bytes, and the index says 1259" | The first entry's file with a byte more. |
| `file-missing` | synth | refuses: "Placeholder Bass: its file is not there" | The first entry's file is not there. |
| `xz` | synth | refuses: "Placeholder Bass: it is compressed with xz, which the plugins read on macOS alone: use zlib" | The first project's file says xz (method byte 2), its hash and size in the index right. The bytes after the header are zlib's: a plugin refuses on the method byte, before inflating. |
| `two-versions` | synth | refuses: "Placeholder Bass: it is not one version" | The first project has two versions. |
| `version-number` | synth | refuses: "Placeholder Bass: its version is not numbered 1" | The first project's one version is numbered 2. |
| `project-id` | synth | refuses: "Placeholder Bass: it holds a project id, which names chats on one machine: a factory project has none" | The first project has an id. |
| `midi` | synth | refuses: "Placeholder Bass: it holds MIDI mappings (someone's controller's)" | The first project's version has MIDI mappings. |
| `samples` | synth | refuses: "Placeholder Bass: it holds samples: a factory project is text alone" | The first project carries sample files. |
| `version-samples` | synth | refuses: "Placeholder Bass: its version plays samples: a factory project is text alone" | The first project's version plays samples. |
| `name-inside` | synth | refuses: "Placeholder Bass: its name inside is "Fixture Other Name"" | The first project's name inside its file is not its entry's. |
| `project-kind` | synth | refuses: "Placeholder Bass: it is a project of kind effect, not instrument" | The first entry's file is the effect's project, renamed: a project of the other kind in a good synth index. |

A refused set leaves the set already on the machine as it was. A case's reason names the project at fault first
(`<name>: ...`) when it is one project's.
