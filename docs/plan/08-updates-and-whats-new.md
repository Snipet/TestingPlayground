# 08. The offer, the update check, What's new

Planning-document items: "Add a way to detect updates and prompt the user (can be turned off in some settings
menu)" and "Add a changelog that appears once (retriggerable from the settings menu) after first opening the
app/plugin". Size L, in three pull requests here and one in the website repository. Decisions D5, D6, D7. Needs 07
(the Settings card).

## 1. What exists

- **No address says what the latest version is.** A `v*` tag makes a draft release on GitHub (`release.yml`);
  publishing it copies the installers to the website's bucket and asks the website to offer it
  (`publish-downloads.yml`); the website's `scripts/offer-release.ts` writes `src/lib/release.json` (version, tag,
  date, each file's name, size and SHA-256), which is compiled into its pages (`src/lib/release.ts`). Nothing
  serves it as data. A pre-release stops before the website.
- **The pattern for reaching the website** is the factory store (`Source/plugin/FactoryStore.h/.cpp`): one object
  a process, held by every processor (`attach()`), its own thread, joined when the last processor goes; it fetches
  only after an editor has opened, after a yes, with `<PREFIX>FACTORY` not `off`, at most once a day; one fixed
  site (`kSite`), no redirect followed, a `User-Agent` of name and version and nothing else
  (`claude::Transport::getLimited`); the transport and the clock injected for probes. It has `versionOf` and
  `newEnough` for comparing versions.
- **The offer is not built.** The factory spec (website repo, `docs/factory-projects.md`, sections 6.2 and 7)
  describes a card shown once, the first time an editor opens: "Get the factory projects?" with "Get them" and
  "Not now"; nothing calls `FactoryStore::editorOpened()` or sets its answer yet (its step 3).
- **The version** is `product::kVersion` (`products/<key>.cmake`, both products the same: 0.1.0).
- **CHANGELOG.md** is by day: `## Unreleased`, then `## YYYY-MM-DD` sections, each with `### <title> ([#N](url))`
  and **Added** / **Fixed** / **Changed** lists. No heading names a version. About 21 KB.
- **Text is compiled in** by CMake for the examples, the library and the factory projects (`file(READ)` into a
  generated header of raw string literals, `CMAKE_CONFIGURE_DEPENDS`; a literal over 64 KB needs the pedantic
  warning turned off around it).
- The chat draws light markdown (`ChatView::layoutMarkdown`: paragraphs, lists, headings, bold, code).
- README and the download page promise that the plugin uses the network only for the chat (and, in the spec, the
  factory projects after a yes).

## 2. Design

### 2.1 One offer, two switches (D6)

The factory spec's offer card gains a second line, and this package builds the card for both:

> **Use the website?**
> TestingPlayground can get two things from testingplayground.seanfunk.xyz. Nothing about you is sent.
> [x] The factory projects, kept up to date
> [x] A note when a new version is out
> [ Allow ]  [ Not now ]

- Shown once, the first time an editor opens for a user who has answered neither. A card in the editor, not a
  dialog: the plugin plays and edits behind it (the spec's rule). Never without an editor.
- "Allow" stores a yes for each ticked line and a no for each unticked one; "Not now" stores nothing-asked-again
  for both (the spec's "Not now": remembered, never offered again by itself). Each can be changed in Settings'
  Network page, which is where the offer can be made again.
- The answers live with the factory's, in the folder both products share (`Funk/Playground/`): `factory.json`
  keeps its `answer`; a new `app.json` holds `{"updates": "yes" | "no" | "later", "skipped": "0.3.0",
  "seen": "0.2.0", "checkedMs", "latest": {...}, "prices": {...}}`, written beside and renamed in under the
  folder's `.lock`, as the factory state is.
- This package calls `FactoryStore::editorOpened()` and sets its answer. The FACTORY group's states and removing
  the compiled-in set remain the factory spec's own step.

### 2.2 The website's endpoint (D5; in TestingPlaygroundBackend)

`GET /app/v1/release.json`, a static file made at build from `src/lib/release.json` and a new
`src/lib/prices.json`:

```json
{
  "format": "funk-playground-release", "version": 1,
  "latest": { "version": "0.2.0", "tag": "v0.2.0", "date": "2026-11-20", "summary": "One or two lines." },
  "prices": { "asOf": "2026-11-03", "webSearchPer1000": 10,
              "usdPerMTok": { "anthropic": { "claude-opus-5-5": { "in": 4, "out": 20, "cacheRead": 0.2,
                              "cacheWrite5m": 5, "cacheWrite1h": 8, "fastIn": 8, "fastOut": 40 } } } }
}
```

- Served with `cache-control: public, max-age=300` and `content-type: application/json`, no redirect.
- `summary` comes from the release's blog post's first paragraph or a field the release workflow passes; empty is
  fine. `prices` is package 09's (section 3.3 there); the endpoint is one so that one daily request serves both.
- Only published, non-pre-release versions get there, as the download page (`offer-release.ts`).
- Not signed: the plugin never follows an address from it (it opens its own fixed download page) and never runs
  anything from it, so a forged copy can only nag or stay silent. The factory index is signed because it is code.

### 2.3 The check

`Source/plugin/UpdateCheck.h/.cpp`, the factory store's shape: one a process, `attach()` from every processor, its
own thread joined with the last, nothing in a constructor, `prepareToPlay` or the audio thread.

- It fetches `kSite` + `/app/v1/release.json` (64 KB at most, no redirect, the `User-Agent` alone) when: an editor
  has opened; the answer is yes; `<PREFIX>UPDATES` is not `off` (the probes' environment sets it,
  `cmake/Probes.cmake`); and the last check is a day old, or the user pressed Check now.
- `<PREFIX>FACTORY_SITE` points it at a developer's site too (the same rule: https, or http to this machine).
- It publishes `State { status (off, unanswered, idle, checking, failed), latest {version, date, summary},
  newer, checkedMs, why }` and hands `prices` to package 09's table. `newer` is `latest.version` above
  `product::kVersion` by `versionOf`'s order (a pre-release build, `0.2.0-rc.1`, is below `0.2.0`).
- A reply that does not read, is too large, names another format, or a failed connection: `failed` with why in
  words, the last good `latest` kept, tried again the next day. Never an error shown unasked.
- `EditorPort`: `updateState()`, `checkForUpdates()`, `setUpdates(bool)`, `skipUpdate(version)`.

### 2.4 Telling the user (assumption A5)

- A pill in the top bar, "0.2.0 is out", while `newer` and that version is not skipped; and the status line says
  it once a process ("VERSION 0.2.0 IS OUT").
- A click opens the **update card**: "TestingPlayground 0.2.0 is out. You have 0.1.0.", the summary, then
  **Download** (opens `kSite` + `/download` in the browser: a fixed address, through `plugin/Launch.h` on Linux,
  never a shell), **Skip this version**, **Later**.
- Nothing opens by itself, in the Standalone or in a host: a session must never be interrupted by a card.
- Settings, Network page: the switch "Check for updates" (the same answer), "Checked today at 14:02: you have the
  latest" or "0.2.0 is out", **Check now**, and the line "Sends the plugin's name and version to
  testingplayground.seanfunk.xyz, nothing else."

### 2.5 What's new (D7, assumption A6)

- **The changelog by release.** A release gets a heading of its own above the days it holds:
  `## Version 0.2.0 (2026-11-20)`. Everything under it, to the next such heading, is that version's. The days
  before the first one are 0.1.0's (one heading added once, at the right place). `## Unreleased` stays on top and
  becomes the next version's heading at release. `release.yml` refuses a tag whose version has no heading.
- **Compiled in:** CMake reads CHANGELOG.md into a generated `Changelog.h` (cut into literals under 64 KB),
  `CMAKE_CONFIGURE_DEPENDS` on it. `Source/plugin/Changelog.h/.cpp` (JUCE-free): `changelog::versions()` gives
  each version's number, date and text; `changelog::since(seen, current)` what the card shows. A development
  build whose version has no heading shows Unreleased.
- **The card:** "What's new in 0.2.0", a scrolling page of the markdown (`ChatView`'s `layoutMarkdown` moved to
  `Source/editor/Markdown.h/.cpp` and used by both; a link is shown as its text and opens only an `https://`
  address of this repository's or the website's), Close. When several versions were skipped, each under its own
  heading, the newest first.
- **Once:** the first time an editor opens with `app.json`'s `seen` not this version, after the offer card is
  answered (never two cards at once), and never over another card or in a headless Panel. Opening it, by itself
  or by hand, sets `seen`. A first install is a version never seen: it shows once there too, as the planning
  document says.
- **Again:** Settings, General: "What's new".
- The probes' environment sets `<PREFIX>WHATSNEW=off` so no other probe meets the card; its own probe turns it on.

## 3. Work

1. **Website:** the endpoint, `prices.json`, a test that the file is what `release.json` says, its README.
2. **Here, the offer and the check:** `app.json`; `UpdateCheck`; the offer card; the pill, the card, the status
   line; the Network page; the factory store's answer and `editorOpened()` called.
3. **Here, What's new:** the headings in CHANGELOG.md; the generated header; `Changelog`; `Markdown` out of
   `ChatView`; the card; the General page's button; `release.yml`'s check.

## 4. Tests

- New probe `plugin.updates`, over a scripted transport and clock: nothing is asked before an editor opens,
  without a yes, with the switch off, or with `<PREFIX>UPDATES=off`; one request with a `User-Agent` alone;
  newer, the same, older; a pre-release build against its release; a day later, and not before; Check now; a
  redirect not followed; a body past the limit, a broken one, another format: `failed`, the last good kept;
  skipped; two processors, one check; a processor going mid-fetch joins promptly; `app.json` written under the
  lock by two stores at once (neither's answer lost); the prices handed on.
- New probe `plugin.changelog`: the real CHANGELOG.md parses into versions (each has text, the newest is this
  build's or Unreleased); `since` for a seen version, for none, for several skipped; a made-up changelog with odd
  headings.
- New probe `gui.updates`: the offer (both lines ticked, one unticked, Allow, Not now, never again, again from
  Settings; it stops nothing: the editor takes keys behind it); the pill, the status line once, the update card
  and its three buttons (Download goes to the launcher's hook, not a browser); the Network page; What's new once
  per version, after the offer, never over a card, again from Settings, links; both themes, the least window.
- `plugin.factorystore`: its rows for "nothing asked before an editor opens or without a yes" hold with the
  offer's answer set through the new path.
- `gui.chat`: unchanged and green (the markdown move changed nothing).

## 5. Docs

- README: the network promise rewritten (what reaches the network, when, what is sent: the chat; after a yes the
  factory projects and the update check; git when asked, package 05); "Updates" and "What's new"; the developers'
  switches (`<PREFIX>UPDATES=off`, `<PREFIX>WHATSNEW=off`). The website's download page says the same.
- CHANGELOG: the `## Version` headings themselves, and Added.
- CLAUDE.md: "The network" rewritten to list every reacher and its rule; rows for `UpdateCheck`, `Changelog`,
  `Markdown`; the release recipe gains "give CHANGELOG.md its `## Version` heading".

## 6. Risks

- **The promise.** Three things now reach the network besides the chat. The README, the download page and
  CLAUDE.md must say exactly which, and every one stays off until a yes or a press.
- **A host scanning plugins** must never see any of this: every path starts from an editor opening, as the
  factory store's does. `plugin.updates`' first rows are the guard.
- **Both products at once** (a host with the effect and the synth): one `app.json`, written under the lock; the
  card shows in whichever editor opens first and sets `seen` for both.
