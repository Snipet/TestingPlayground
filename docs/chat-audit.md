# Audit of the AI chat integration

*2026-11-11, at commit `73b677d` (main).* A sweep of the chat with Claude for bugs and inaccuracies: the agent's turn
loop, the API client and the other model providers, the key and chat stores, the tools Claude calls, the Claude Desktop
(MCP) side, and the chat panel's interface. This is a report only: no code was changed. Line numbers are that commit's.

## Summary

- Nine reviewers raised **103 candidates**; with duplicates merged, **90 distinct findings**. Every
  one then went through an independent second pass whose brief was to refute it: **84 were confirmed**
  (3 high, 20 medium, 61 low), **5 are plausible but not proven**, and **1 was
  refuted**.
- Ten of the confirmed findings were reproduced by running the real code (TURN-1, TURN-2, API-1, FILES-3, FILES-7,
  TOOLS-2, UI-1, UI-2, UI-3, UI-24), eight more by a small harness (STORE-1, LINK-4, LINK-5, SERVER-2, SERVER-6,
  SERVER-9, API-5, FILES-8), and two are visible in the rendered probe pictures (UI-4, DOCS-3). The others rest on the
  second reader's own trace of the code.
- Nothing was found that sends a chat or a key to a place the user did not name: the stores' cryptography, the origin
  binding of provider keys, the "references only" rule of the session and the live tools' thread contract all held
  (see "Considered and refuted, and what held"). Two findings sit at that rule's edge: the key dialog can name one
  server while the key goes to the one typed (UI-18), and a zero-padded address passes as a local one (API-5).

The three high findings:

1. **TURN-1.** Pressing Stop (or a stream that breaks off for good) while the API is writing a compaction summary leaves
   a compaction block with no summary in the history and drops every message before it. The panel still shows the
   conversation; Claude no longer has it, and the chat's file is rewritten that way.
2. **TURN-2.** A reply that ends normally with no content (which the API can return after tool results,
   and which other providers return for a reasoning-only answer) is stored as an assistant message with empty content.
   Every later request of that chat to Anthropic carries it, and the Messages API rejects such a request.
3. **API-1.** On macOS and Windows the transport hands JUCE a POST URL with a query. JUCE moves the query into the body,
   so every Google Gemini turn goes out as `POST ...:streamGenerateContent` with the body `alt=sse{...}`. Gemini's
   models list (a GET) still loads, so the provider looks set up and then fails on the first message.

Where the medium findings cluster:

- **What goes into the history is not checked.** TURN-1 and TURN-2 above, and in smaller ways TURN-8 and TURN-13: a
  partial or empty reply is appended as it came. One guard before `appendAnswer` (and once in `repairHistory`, for chats
  already kept) covers them.
- **The user's work and the user's key.** A failed disk write replaces a key or chat with an empty file and says it was
  saved (STORE-1); an edit of Claude's can replace what was typed in the same file within one frame (FILES-1);
  non-ASCII text Claude writes turns into "?" and is then journalled as the user's edit (FILES-2); the key dialog names
  one server while the key is bound and sent to another (UI-18) and draws up to eleven characters of any key (UI-4); a
  31 KB picture costs 1.7 GB of memory off macOS (FILES-3).
- **The Send/Stop button.** It is one button that acts on press and keeps the keyboard focus, and at the least width
  another control lies over it: a double-click stops the message it just sent (UI-1), Space after a click stops the
  turn (UI-2), and a click on Stop toggles web search instead (UI-3).
- **Two Claudes on one instance.** The plugin's chat and Claude Desktop share `busy_`, the file-versions record, the
  cancel slot and the journal's attribution, each with a small gap (TURN-3, TURN-12, LINK-2, LINK-3, LINK-9, STORE-5,
  STORE-6).
- **Words for the Mac on Windows and Linux.** About twenty strings shown to the user or to Claude say "this Mac",
  "Cmd" or "/Users/..." without going through `kThisMachine` and `shortcutText` (DOCS-3).

| ID | Severity | Finding | Evidence |
| --- | --- | --- | --- |
| TURN-1 | high | A Stop (or a broken stream) during compaction drops the conversation's history | run |
| TURN-2 | high | An empty reply is stored as an empty assistant message, which every later Anthropic request carries | run |
| API-1 | high | Gemini requests are malformed on macOS and Windows: the query is moved into the body | JUCE's source; mechanism run on Linux |
| STORE-1 | medium | A failed write replaces a key or a chat with an empty file and reports success | harness |
| FILES-1 | medium | edit_file and edit_circuit can overwrite what the user typed in the same file a moment before | independent trace |
| FILES-2 | medium | Non-ASCII text that Claude writes becomes "?" in the editor and then "the user's edit" | independent trace |
| FILES-3 | medium | A tiny picture file makes the plugin allocate gigabytes (Windows and Linux) | run |
| UI-1 | medium | A double-click on Send sends the message and stops the turn | run |
| UI-2 | medium | After a click on Send, Space or Return stops the turn | run |
| UI-3 | medium | At the least window width the Web pill covers the Send/Stop button | run |
| UI-18 | medium | After the Server field is edited, the key dialog still names the preset's server | independent trace |
| UI-4 | medium | The key field shows up to eleven characters of any key | trace and pictures |
| TURN-3 | medium | A message sent just as Claude Desktop starts a call runs a turn beside it | independent trace |
| TURN-14 | medium | A message sent while pictures are still being read goes without them | independent trace |
| TURN-5 | medium | A models list that failed once is never asked for again | independent trace |
| TURN-4 | medium | Every instance keeps its last 64 request bodies in memory | independent trace |
| API-3 | medium | On macOS an answer arrives in 8 KB jumps instead of as it streams | independent trace |
| STORE-3 | medium | Chats that cannot be unsealed are listed, and can be neither opened nor deleted | independent trace |
| TOOLS-1 | medium | A recording's replay check compiles without the program's samples and reports a false difference | independent trace |
| TOOLS-2 | medium | A note or a glide that ends past a render's time cap is cut silently, and the result states the times asked | run |
| DOCS-2 | medium | capture is announced to MCP clients as read-only, though arming it changes what plays | independent trace |
| SERVER-6 | medium | The GUI sandbox leaves a folder's netlists out, so a circuit program has no circuit dials there | harness |
| UI-24 | medium | A window opened while the chat is folded asks for a frame on every tick | run |

## How to read this

- **IDs** name the area a finding came from: TURN (turn loop and conversation state), API (API client and other
  providers), STORE (key and chat stores, chat ownership), FILES (file tools, projects, pictures), TOOLS (measuring
  and debugging tools), LINK (the plugin's end of the Claude Desktop connection, live tools, journal), SERVER (the MCP
  server and the GUI sandbox), UI (the chat panel), DOCS (descriptions, annotations and strings against the code).
  Numbers are the reviewers' own, so some are missing here: a duplicate is listed under the finding it merged into.
- **Severity.** High: the conversation or the user's work is lost, a key or chat is exposed, the plugin crashes or
  hangs, or an ordinary request is rejected. Medium: wrong behaviour a user or Claude would notice on a plausible path.
  Low: wording, a documentation inaccuracy, or a rare edge with a mild outcome.
- **Evidence.** *Reproduced by a run*: the plugin's own code was run (a Linux build, the scripted API) and did it.
  *Reproduced by a harness*: a small standalone program around the real source did it. *Independent trace*: a second
  reader, told to refute the finding, followed the failing path through the code and could not. *Plausible*: the
  mechanism is real but reaching it, or the peer's reaction, could not be settled here.

## High

### TURN-1. A Stop (or a broken stream) during compaction drops the conversation's history

**Severity:** high. **Where:** `Source/plugin/ChatAgent.cpp:3145`. **Evidence:** reproduced by a run.

**What is wrong.** The path that ends a turn on a stop, an incomplete stream or a refusal keeps every block of type
"compaction" from the partial reply without checking that its summary arrived. MessageBuilder::content() returns a
compaction block as soon as its content_block_start came (the summary comes later, whole, in one compaction_delta), so
appendAnswer() takes the reply for a compacted one and repairHistory() removes every message before it. The API
history becomes [assistant: {type: compaction, no content}, "(stopped by the user)"], seen_ is cleared, and keep()
writes that to the chat's file at the turn's end.

**Scenario.** A chat past 150K input tokens on a model with compaction. The next request starts compacting:
content_block_start {type: compaction} arrives, then nothing while the summary is written. Press Stop. messages_ is
now [assistant: [compaction without a summary, "(stopped by the user)"]]; every message before it is gone from memory
and, after keep(), from the chat's file, while the panel still shows the conversation. The next message goes to Claude
with no history (or is refused, if the API does not take the block or a history that starts so).

**How it was checked.** Reproduced with the scripted API (`fake::Transport`): after two ordinary exchanges, a third
request whose stream held only `message_start` and `content_block_start {"type":"compaction"}` was stopped. The saved
history went from four messages to one,
`[{"role":"assistant","content":[{"type":"compaction"},{"type":"text","text":"(stopped by the user)"}]}]`, and the
next request began with that block.

**Suggested fix.** Keep a compaction block from a partial reply only when its summary arrived (content a non-empty
string), and have appendAnswer and repairHistory cut the history only at such a block; MessageBuilder should not turn
a null summary into "".

### TURN-2. An empty reply is stored as an empty assistant message, which every later Anthropic request carries

**Severity:** high. **Where:** `Source/plugin/ChatAgent.cpp:3180`. **Evidence:** reproduced by a run. Also found as
API-2.

**What is wrong.** When a reply finishes normally (message_stop, no error, not a refusal) with no blocks, runTurn
appends {"role":"assistant","content":[]} and ends the turn silently. For another provider this is ordinary: its
reasoning is removed before the reply is stored, so a reply of reasoning alone (or an empty completion) leaves no
block. The OpenAI and Gemini translations and the planner's view each put "(no answer)" in such a message's place, but
the request to Anthropic sends messages_ as it is, and the Messages API refuses a message with empty content that is
not the last one.

**Scenario.** With fake::Transport: one reply `Sse().start().end()` (message_start, message_delta end_turn,
message_stop). The chat shows nothing; saveState()'s last message is {role: assistant, content: []}. Send one more
message: the request's messages hold that empty assistant message between two user messages. Real life: a local model
on Ollama answers a tool round with reasoning only and finish_reason stop, then the user chooses Anthropic again in
the same chat.

**How it was checked.** Reproduced with the scripted API: a reply of `message_start` ... `message_stop` with no
content block left `{"role":"assistant","content":[]}` in the history, and the next request carried it between two
user messages. That the API answers 400 to such a request ("all messages must have non-empty content except for the
optional final assistant message") is general knowledge of the Messages API; the bundled reference does not state it,
and no live call was made.

**Suggested fix.** Never append an assistant message with no content: put a "(no answer)" text in its place at 3180
(as the stop path and the other providers' translations do) and say so in the chat; also mend an empty one in
repairHistory for chats already kept.

### API-1. Gemini requests are malformed on macOS and Windows: the query is moved into the body

**Severity:** high. **Where:** `Source/plugin/ClaudeApi.cpp:263`. **Evidence:** settled from JUCE's source for macOS
and Windows; the mechanism reproduced by a run on Linux.

**What is wrong.** The non-Linux transport builds a juce::URL from host + path and opens juce::WebInputStream(url,
post). juce::URL's string constructor parses everything after '?' into parameters and takes it off the address (JUCE
documents URL::createWithoutParsing for exactly this), and WebInputStream's second argument is
addParametersToRequestBody: true for a POST, so the parameters are written into the request body, ahead of the POST
data, and left out of the address. The one POST path with a query is Gemini's (Providers.cpp:862), so on macOS and
Windows a Gemini turn is sent to .../models/&lt;m>:streamGenerateContent with no ?alt=sse and a body that starts
"alt=sse{..." , which is not JSON. The Linux libcurl transport passes host + path through untouched and is not
affected; the GETs with a query (/v1/models?limit=100, /v1beta/models?pageSize=1000) pass false and keep their query.

**Scenario.** macOS or Windows build, provider Google Gemini with a valid key, any model: send "hello". The request
goes out as POST .../models/&lt;m>:streamGenerateContent with body alt=sse{...}; the turn ends with "Google Gemini:
HTTP 400: ...". Every Gemini turn, on two of three platforms.

**How it was checked.** Settled from JUCE 8.0.4's source for the macOS and Windows backends (`juce_URL.cpp`'s `init`
and `createHeadersAndPostData`; `juce_Network_mac.mm:981, 1013`; `juce_Network_windows.cpp:94, 318`). The same two
calls are in JUCE's Linux backend, so the mechanism was reproduced there by a run of the transport's exact calls
against a local socket: the request line was `POST /v1beta/models/gemini-x:streamGenerateContent HTTP/1.1` and the
body `alt=sse{"probe":true}`. Not run on macOS or Windows, and no live call to Gemini was made. No existing probe
posts the real transport to a path with a query.

**Suggested fix.** Build the URL with juce::URL::createWithoutParsing(host + path) (then both GET and POST keep the
address as written and WebInputStream(url, post) has no parameters to move), or keep the parsed URL and pass false as
WebInputStream's second argument (POST data alone makes it a POST: hasBodyDataToSend).

## Medium

### STORE-1. A failed write replaces a key or a chat with an empty file and reports success

**Severity:** medium. **Where:** `Source/plugin/ChatStore.cpp:294`. **Evidence:** reproduced by a harness.

**What is wrong.** writePrivate() treats juce::FileOutputStream::write() returning true as 'written', but JUCE's
stream only copies anything smaller than its 16384-byte buffer into memory and returns true; the real write(2) happens
in flush(), whose result (the stream's status) is never looked at. On a full disk, a quota or an I/O error the
temporary is left empty or cut short, is then moved over the target (whose old copy moveFileTo deletes first), and
every caller (saveKey, saveProviderKey, saveChat, storeSalt, saveChatJson) returns success.

**Scenario.** The disk is full. The user saves an API key (or a turn ends in a short chat): writePrivate creates
&lt;file>.tmp, write() only buffers, flush()'s write(2) fails unseen, moveFileTo deletes the good file and renames the
empty temporary over it; saveKey returns true ('API key saved, encrypted'), saveChat SaveResult::ok. The next start
cannot read the key; the chat on disk is 0 bytes.

**How it was checked.** Confirmed from JUCE 8.0.4's `FileOutputStream` (a write under the 16 KB buffer only buffers;
the flush's result is not read here) and by a harness of the same system-call sequence under a file-size limit.

**Suggested fix.** Open the stream unbuffered (juce::FileOutputStream out(temp, 0): write() then returns the real
result) or, after flush(), require out.getStatus().wasOk() and the temporary's size == n; delete the temporary on
every failure path; replace with rename(2) (STORE-2) so the old file survives a failure.

### FILES-1. edit_file and edit_circuit can overwrite what the user typed in the same file a moment before

**Severity:** medium. **Where:** `Source/editor/Panel.cpp:5536`. **Evidence:** confirmed by an independent trace.

**What is wrong.** chatSetDraftIf compares the file with the PROCESSOR's draft, but the editor gives typing to the
processor only at the next Panel::refresh (a frame later); only schematic gestures are given at once (the sync hook).
A keystroke typed in a tab in the same frame interval in which Claude's edit_file (or a Netlist-view edit_circuit) of
that file lands is therefore invisible to the guard, the write succeeds, and Panel::mergeDraft then replaces the tab's
text with Claude's because the draft 'did not leave' that file. CLAUDE.md promises the opposite for edit_file ('what
the user typed meanwhile is never overwritten').

**Scenario.** Editor open on program.fm; a turn calls edit_file on program.fm. The user types '5' (0.2 -> 0.25) in
program.fm; before the next frame the worker's chatSetDraftIf lands (draft_ still holds 0.2, equal to what the tool
read). Next refresh: mergeDraft replaces the tab with Claude's text; the '5' is gone, the tool said 'Edited
program.fm', nothing mentions it. Cmd-Z restores the '5' and drops Claude's edit from the tab.

**Suggested fix.** Give typing to the processor in the event that made it, as the schematic's sync hook does (call the
same setDraftIf from the Panel's key/text path when files().revision() != syncedFiles_), so chatSetDraftIf sees it and
re-runs the edits; or have mergeDraft refuse to replace a tab typed in since the last push when the draft also changed
that file (push the tab's text and let the tool's next call see it).

### FILES-2. Non-ASCII text that Claude writes becomes "?" in the editor and then "the user's edit"

**Severity:** medium. **Where:** `Source/plugin/ChatImport.cpp:521`. **Evidence:** confirmed by an independent trace.

**What is wrong.** Only write_file's `from` runs TextEditor::sanitise and says so. Text Claude sends itself
(write_file's content and files, edit_file's new_text) goes into draft_ as it is: chatSetDraft's tidyFiles tidies
names only, chatSetDraftIf nothing. The editor holds printable ASCII, tabs and LF, so a tab shows '?' for every other
character while read_file, run_program, seen_ and the version all use the raw text. The first time the user types
anywhere, Panel::pushFiles writes every tab's sanitised text back, which changes Claude's file in the draft and is
journalled as the user's change. Nothing in the prompt tells Claude that files are ASCII (only that chat replies are).

**Scenario.** Editor open. write_file {name: "program.fm", content: "// cutoff \u2248 2 kHz\n..."}; run_program. The
tab shows '// cutoff ? 2 kHz' and the edited light is on. The user clicks another tab or types one character anywhere:
the draft's program.fm becomes the '?' text, journalled as the user's edit; edit_file {name: "program.fm", expected:
&lt;the version write_file gave>, ...} is refused as the user's edit.

**Suggested fix.** Sanitise in one place, where Claude's text enters the draft (chatSetDraft and chatSetDraftIf, or
the tools before them, with the same TextEditor::sanitise the editor applies), keep the version and seen_ of the
sanitised text, and say it in the result as `from` does ('N characters outside ASCII became ?'); add a line to the
prompt that files are ASCII.

### FILES-3. A tiny picture file makes the plugin allocate gigabytes (Windows and Linux)

**Severity:** medium. **Where:** `Source/plugin/ChatImages.cpp:231`. **Evidence:** reproduced by a run.

**What is wrong.** fromFile caps the file's bytes (kMaxFileBytes, 64 MB) but nothing caps its pixels. On Windows and
Linux (and on macOS when ImageIO gives nothing) fromBytes hands the bytes to juce::ImageFileFormat::loadFrom, which
allocates width x height from the header and decodes it all, and only then scales to kMaxEdge. A highly compressible
PNG, JPEG or GIF of tens of thousands of pixels a side is a few MB on disk and 5 to 17 GB decoded; JUCE's loaders use
non-throwing HeapBlocks and do not check them, so a failed allocation is a write through a null pointer, and a
'successful' overcommitted one is the OOM killer, in the DAW's process.

**Scenario.** Linux or Windows build: the user drops a 321-byte PNG whose header says 1,000,000 x 1,000,000 on the
chat. The size check passes, JUCE's loader asks for 4 TB, malloc fails, and libpng writes the second row at address
4,000,000: the host process dies. With a 110 KB 30,000 x 30,000 one-colour PNG nothing fails: the job thread touches
about 6.3 GB before it scales the picture to 1568 px.

**How it was checked.** Reproduced on Linux: a 31,190-byte PNG (16,000 x 16,000 pixels of one colour) attached to the
chat raised the process's peak resident memory by 1,706 MB in 2.2 s, and was accepted. The larger cases (a 110 KB file
decoding to 6.3 GB; a 321-byte header that asks for 4 TB and is written through after the failed allocation) come from
the trace of JUCE's loaders and were not run.

**Suggested fix.** Read the dimensions from the header before decode() (PNG IHDR bytes 16-23, JPEG SOFn, GIF logical
screen) and refuse past a pixel count (say 100 MP) in words, as the other refusals do; the macOS path can check
ImageIO's kCGImagePropertyPixelWidth/Height the same way.

### UI-1. A double-click on Send sends the message and stops the turn

**Severity:** medium. **Where:** `Source/editor/Panel.cpp:3239`. **Evidence:** reproduced by a run.

**What is wrong.** The round button is Send while idle and Stop while a turn runs, it acts on press
(ButtonItem::pointerDown ignores e.clicks), and sendChat() refreshes chat_ at once, so chat_.busy is already true when
the second press of a double-click arrives. That second press calls ChatPort::stop(): the request just sent (billed)
is cancelled and the chat says Stopped.

**Scenario.** Idle chat with a key, text in the message box: double-click the round Send button. Press 1 sends (the
icon becomes Stop under the pointer), press 2 calls stop(): the user's message is in the history, the request is
cancelled, the chat says 'Stopped.'.

**How it was checked.** Reproduced in a headless editor with the scripted API: after `doubleClick` on Send the
transcript ended with the user's message and nothing after it, the agent was idle, the message box was empty, and no
request had been posted. In this run the second press landed before the first request, so there was not even a
"Stopped." note (TURN-11); with a slower second press the request is sent and then cancelled.

**Suggested fix.** Let the send item ignore a press with e.clicks > 1 (or any press within the double-click time of
the one that sent), as closingPress_ does for cards: the press that sent must not be followed by a stop from the same
double-click.

### UI-2. After a click on Send, Space or Return stops the turn

**Severity:** medium. **Where:** `Source/editor/Panel.cpp:8313`. **Evidence:** reproduced by a run.

**What is wrong.** Any press on a chat button moves the Panel's focus to that button (the message box loses its caret)
and sendChat() does not give it back. With the focus on the Send/Stop button and no focus ring drawn (ring_ is false
after a pointer press), Space and Return run the button's action, which during a turn is stop(); every other key falls
through to the host.

**Scenario.** Type a message, click the round Send button with the mouse, then press Space while Claude works (the DAW
user's play key, or the first space of the next message): the turn stops with 'Stopped.'. Letters typed instead go to
the host, not the message box.

**How it was checked.** Reproduced in a headless editor: after a click on Send the turn was streaming and the message
box no longer had the focus; one Space ended the turn with "Stopped.".

**Suggested fix.** After a send from the button give the focus back to the message box (setFocus(inputItem_) in
sendChat, as suggest() does), or do not move the focus to the send item on a pointer press.

### UI-3. At the least window width the Web pill covers the Send/Stop button

**Severity:** medium. **Where:** `Source/editor/Panel.cpp:3881`. **Evidence:** reproduced by a run.

**What is wrong.** placeChatBar() narrows the thinking pill and then the model pill, but never below 64 px each and
never the Fast and Web pills. When the bar has less than 132 px for the two (a chat under about 355 px wide with a
model that takes both fast mode and web search, e.g. the least window of 1000 px, chat 330 px) the row overflows and
the Web pill's bounds cover most of the Send button. itemAt() returns the first item that contains the point and the
Web pill comes before Send in items_, so a click on Send (or Stop) toggles web search.

**Scenario.** Window 1000 px wide (the least), Anthropic with Claude Opus 5.5 (the default): the chat is 330 px. Start
a turn and click the middle of the Stop button: web search is switched (machine-wide, billed per search) and the turn
runs on. Only the right 13 px of the button stop it.

**How it was checked.** Reproduced in a headless editor at the least window: the accessibility tree put "Web search"
at x 896 to 951 and "Send or stop" at 936 to 964; with a turn streaming, a click on the centre of the Stop button left
the turn running and switched web search from off to on. The repository's own picture `gui.chat.small.png` shows the
pill ("We") under the button.

**Suggested fix.** Make the row fit: let the model and thinking pills shrink further (or drop Fast and Web to their
icons, or hide the thinking label) when room &lt; 132, and clamp so that web.right() &lt;= send.x - 8; hit-testing
Send before the pills would only hide the overlap.

### UI-18. After the Server field is edited, the key dialog still names the preset's server

**Severity:** medium. **Where:** `Source/editor/Panel.cpp:6616`. **Evidence:** confirmed by an independent trace.

**What is wrong.** Every OpenAI-kind preset shows an editable Server field, and Save, Remove key and the key's binding
use the address typed (dialogProvider()). The dialog's title, its line 'sent only to &lt;server>', the hint 'Stored:
empty keeps it' and whether Remove key shows come from the preset's own base and from dialogInfo_, read once when the
preset was chosen. After an edit the words name one server while the key is stored for, sent to, or removed from
another.

**Scenario.** Key dialog, Provider OpenRouter; click Server, Cmd-A, paste https://llm.example.test/v1; click Key,
paste a key. The dialog still reads 'OpenRouter ... It is stored encrypted on this Mac and sent only to
openrouter.ai.'; Return seals the key to https://llm.example.test and the chat sends it there. (With Ollama as the
preset and a remote address typed, the line stays 'no key, and nothing leaves this Mac'.)

**Suggested fix.** Describe dialogProvider(), not the preset: on every change of the Server field read dialogInfo_ =
providerInfo(dialogProvider()) again and word the title, the 'sent only to' line, the hint and Remove key from the
typed base's host (or switch the dialog to the custom preset once the address differs from the preset's).

### UI-4. The key field shows up to eleven characters of any key

**Severity:** medium. **Where:** `Source/editor/Panel.cpp:6581`. **Evidence:** confirmed by trace and by the rendered
pictures.

**What is wrong.** maskedKey() was written for Anthropic keys (the public prefix 'sk-ant-' is 7 characters) and is
used for every provider. For another provider's key the first 7 characters are usually secret, so 7 to 11 secret
characters are drawn in clear, and a key of 12 characters shows 11 of them (one star). CLAUDE.md's rule ('the panel
masks it', the same rules for another provider's key) is not kept for those keys.

**Scenario.** Key dialog, Provider 'Other OpenAI-compatible server', Server https://llm.example.test/v1, Tab to Key,
paste the 12-character key 'token-abc123': the field draws 'token-a\*c123' (11 of its 12 characters) until Save or
Escape.

**How it was checked.** Traced, and visible in the repository's own probe pictures: `gui.providers.openrouter.png`
draws `sk-or-v***...***cdef` and `gui.providers.gemini.png` `AIzaPro***...***defg` (a Gemini key's public prefix is
four characters, so seven secret characters show).

**Suggested fix.** Show as head only the preset's own public prefix (ChatPreset::key) when the text starts with it,
else none; show the last four only for a long key (20 characters or more, say); stars for everything else, also while
it is typed.

### TURN-3. A message sent just as Claude Desktop starts a call runs a turn beside it

**Severity:** medium. **Where:** `Source/plugin/ChatAgent.cpp:1515`. **Evidence:** confirmed by an independent trace.
Also found as LINK-1.

**What is wrong.** send() tests busy_ under mutex_, releases it, joins the last turn's thread (which may still be
writing the chat in keep()) and builds the context block, then takes mutex_ again and delivers without testing busy_
again. mcpCall() (the MCP link's worker thread) claims busy_ under the same mutex in that window, so both run.
answer() has the second test; send() lacks it. When the MCP call ends it sets busy_ = false and clears the activity
and turnThread_ while the chat's turn is still running.

**Scenario.** A turn ends (busy_ false, keep() still writing). The user sends the next message: send() passes its test
and waits in joinWorker(). Claude Desktop's read_file arrives: mcpCall takes busy_. send() goes on, delivers and
starts the turn. The call returns: busy_ false, the chat shows idle with a Send button while the turn streams; the
user types and sends again: the message thread blocks in joinWorker() until the turn is over.

**Suggested fix.** Test busy_ again under the second lock in send(), as answer() does (and return without sending).

### TURN-14. A message sent while pictures are still being read goes without them

**Severity:** medium. **Where:** `Source/plugin/ChatAgent.cpp:1529`. **Evidence:** confirmed by an independent trace.
Also found as UI-19.

**What is wrong.** attach() reads each file on the job thread and counts it in attaching_ until it lands in attached_.
send() neither waits for attaching_ nor refuses while it is above zero: it sends the text with whatever attached_
holds at that instant. The input strip already shows a place for the picture being read, and Panel::sendChat does not
look at ChatState::attaching either, so a message sent right after a drop goes (and is billed) without its picture,
and the picture then waits in the strip for the next message.

**Scenario.** Type "what is wrong in these?", add six photos with the chooser and press Return at once: the message
goes with the one or two already read (or none); the rest stay in the strip and go with the next, unrelated message.

**Suggested fix.** Hold the send while attaching is above zero (the panel keeps the text and sends when the reads
end), or have send() wait for the picture jobs on the worker before it builds the message.

### TURN-5. A models list that failed once is never asked for again

**Severity:** medium. **Where:** `Source/plugin/ChatAgent.cpp:2752`. **Evidence:** confirmed by an independent trace.
Also found as API-9.

**What is wrong.** fetchModels() sets modelsFetched_ before the GET and leaves it set when the GET fails (no
connection, a 429, 529 or 5xx, or the user's Stop during it: status 0). Only saveKey() and setTransport() reset it, so
the list is not read again until the plugin is opened again. The other providers' path resets its flag on a failure
("asked again next turn"); Anthropic's does not.

**Scenario.** The session's first message while the API answers /v1/models with 529 (or the network drops for a
moment, or the user presses Stop in its first moments): the GET is never made again. With claude-fable-5-2 chosen,
every later turn asks for 64000 tokens with no effort and no compaction, and the model menu shows four models.

**Suggested fix.** Leave modelsFetched_ false when the GET did not give a list (any status but 200 and 401), as
fetchOtherModels does; perhaps not more often than once a turn.

### TURN-4. Every instance keeps its last 64 request bodies in memory

**Severity:** medium. **Where:** `Source/plugin/ChatAgent.cpp:2705`. **Evidence:** confirmed by an independent trace.

**What is wrong.** Each request sent copies its whole body (the system prompt, the tools, the full history with every
picture in base64) into requests_, which keeps the newest 64 and is read only by requestsSent(), a probes' accessor.
Nothing clears it (not New chat, not a turn's end), so each plugin instance holds up to 64 request bodies in memory
after one long turn.

**Scenario.** One long turn (64 requests or more) in a chat whose history holds a few view_sound pictures: the
instance then holds 64 copies of the whole request text until it is closed.

**Suggested fix.** Remove requests_ and requestsSent() (the probes do not use them), or record only under a probes'
switch.

### API-3. On macOS an answer arrives in 8 KB jumps instead of as it streams

**Severity:** medium. **Where:** `Source/plugin/ClaudeApi.cpp:313`. **Evidence:** confirmed by an independent trace.

**What is wrong.** The JUCE transport reads the 200 body with stream.read(buffer, 8192) and hands each read to
onChunk. JUCE's macOS WebInputStream::read loops until the buffer asked for is full or the stream ends, so onChunk
runs once per 8,192 bytes of server-sent events, not as they arrive. The header (ClaudeApi.h) and CLAUDE.md's
transport rule ("a 200's body to onChunk as it arrives") say otherwise, and the chat's live view (text, thinking, the
tool being written, "Writing program.fm (12 KB)") is built on it. Only the Linux libcurl transport delivers piece by
piece.

**Scenario.** macOS, Anthropic, any model: ask a one-paragraph question. Fewer than about 8 KB of SSE arrive before
message_stop, so the card says Thinking until the whole answer appears at once; a long answer or a file being written
appears in jumps of about 8 KB of events.

**Suggested fix.** Ask JUCE for little at a time on this path (a read of 1 byte returns as soon as one is there; 64 to
256 bytes bounds the lag to an event or two but can hold the last short event back until more bytes come), or read
NSURLSession's data as delivered; then assert transport.piece_by_piece on every platform.

### STORE-3. Chats that cannot be unsealed are listed, and can be neither opened nor deleted

**Severity:** medium. **Where:** `Source/plugin/ChatStore.cpp:1141`. **Evidence:** confirmed by an independent trace.

**What is wrong.** chatsOf() lists every &lt;32 hex>.chat file of the owner's folder through readMeta(), which falls
back to the title "Chat" and the file's time whenever the .meta does not unseal. It never checks that the chat itself
was sealed here, although ChatStore.h:167-168 promises 'files not sealed here are left out'. Such rows can never be
opened (loadChat fails, and the panel ignores openChat's false) nor deleted (Delete acts on the chat shown only), so
they stay in the list for good.

**Scenario.** Copy ~/Library/Application Support/Funk/Playground/chats and the projects to another Mac (Migration
Assistant). Open a project there: its chat menu lists a 'Chat' row per old chat. Click one: nothing happens (a running
turn is stopped). 'Delete this chat...' deletes the chat shown, never one of these.

**Suggested fix.** Make the code match the header: in chatsOf leave out a chat whose .meta fails its HMAC (keep the
'missing .meta' fallback only when the .chat's own header and MAC check out, or check the .chat's MAC lazily and cache
by stamp); or keep listing them but let the row be deleted and have openChat say why it cannot open.

### TOOLS-1. A recording's replay check compiles without the program's samples and reports a false difference

**Severity:** medium. **Where:** `Source/plugin/ChatCaptured.cpp:96`. **Evidence:** confirmed by an independent trace.

**What is wrong.** scenario::captured() makes the take's fidelity line ("Replay: matches what played bit for bit" /
"differs from ...") by null-testing a replay of the whole take. It assigns the samples to `whole.request.assets` and
then calls capture::replayOf(..., whole, ...), whose first statement is `out = Replay{}`: the assignment is lost, so
nullTest's Session compiles with no samples and every #pragma sample field is empty. For a program that reads a sample
the null test compares silence (or a different sound) with what played and reports a mismatch with a made-up cause;
the line is then kept with the take and repeated by capture stop/status and by every replay's `about`.

**Scenario.** Synth: add kick.wav, run a Voice that plays `#pragma sample s "kick.wav"` from its note-on; capture arm
{mode: next}; play one note; capture stop; measure_output {scenario: {phrase: "capture"}}. The metrics are of the
right replay (with the sample), but the result's recording line says 'its replay differs from 0.00 s ...' (silence
against the kick) instead of 'its replay matches what played bit for bit', and capture status repeats 'Replay: differs
...' for that take from then on.

**Suggested fix.** Assign the samples after replayOf, as line 88 does (`if (replayOf(t, 0, -1, whole, unused)) {
whole.request.assets = host.chatAssets(); fidelity = nullTest(t, whole).line; }`), or give replayOf/nullTest an Assets
parameter.

### TOOLS-2. A note or a glide that ends past a render's time cap is cut silently, and the result states the times asked

**Severity:** medium. **Where:** `Source/plugin/ChatScenario.cpp:124`. **Evidence:** reproduced by a run.

**What is wrong.** The scenario reader bounds a note's `at` and a move's `at` and `over` by the request's longest (10
s for a picture, 300 s for numbers) but never their sum: a note's `len` is taken up to 3600 s and at+over is not
checked. fits() cannot catch it because measure::latestTime clamps each end to `longest` before comparing. The Session
then silently cuts the note-off / the glide's end at the cap, while scenarioText and the timeline's marks print the
unclamped values. CLAUDE.md and ChatScenario.h promise the opposite: a time past its cap is refused in words, "so a
Session never re-times a request the tools take".

**Scenario.** Synth: view_sound {scenario: {phrase: [{note: 48, at: 9, len: 5}]}} is accepted and titled 'A scenario:
a phrase of 1 note (C3) over 14.00 s, then 1.50 s' for a render whose note-off is at 10.00 s. Either product:
view_sound {scenario: {automate: [{dial: &lt;a float dial>, at: 9.5, to: &lt;v>, over: 5}]}} plays the glide in 0.5 s;
measure_output with at: 295, over: 20 plays it in 5 s. Neither is refused nor says it was cut.

**How it was checked.** Reproduced on the synth: `view_sound {scenario: {phrase: [{note: 48, at: 9, len: 5}]}}` was
accepted and described its render as "a phrase of 1 note (C3) over 14.00 s, then 1.50 s, 11.50 s in all".

**Suggested fix.** Refuse the sums in the Reader with capWords (phrase[i]: at + len past r.longest; automate[i]: at +
over past r.longest), or stop clamping inside measure::latestTime so that fits() refuses them; if cutting a held note
at the cap is meant to be allowed, say so and make scenarioText and the marks print the cut times.

### DOCS-2. capture is announced to MCP clients as read-only, though arming it changes what plays

**Severity:** medium. **Where:** `Source/plugin/ChatAgent.cpp:3719`. **Evidence:** confirmed by an independent trace.
Also found as TOOLS-9.

**What is wrong.** kFaces marks capture readOnly true, so Claude Desktop receives readOnlyHint:true (the tool 'does
not modify its environment') and may run it without asking. capture arm with restart:true recompiles the program that
plays and cuts what still sounds. Any arm also switches the Engine to the program's own code (the frozen copy is
dropped and not taken again) and holds the freezer. The tool's own description says 'restart ... cuts what still
sounds, so ask the user first', which contradicts the read-only annotation.

**Scenario.** The user holds a chord in the DAW; Claude Desktop calls capture {action: "arm", mode: "next", restart:
true}, a tool announced as read-only. The processor recompiles what plays, the held voices are cut and started again,
REC shows, and the frozen copy stays off until the recording ends.

**Suggested fix.** Set capture's readOnly to false in kFaces (destructive false, or true since arm and forget discard
the take kept); add capture to plugin.mcp's annotation rows.

### SERVER-6. The GUI sandbox leaves a folder's netlists out, so a circuit program has no circuit dials there

**Severity:** medium. **Where:** `Source/sandbox/Sandbox.cpp:353`. **Evidence:** reproduced by a harness.

**What is wrong.** readPath takes only .fm, .gui, .sc and .sh from a folder. The sandbox's dials are documented to
include the circuits' 'as the plugin makes them' (lang::circuitDials), and readProgram does read .cir files when it is
given them (the instance path, which plugin.sandbox CIRCUIT pins), but circuitDials only binds netlists that are
program files, so a folder's own or copied .cir is never seen: the pots and switches are missing, every control on
them is reported as naming no parameter, and the picture is not the plugin's. The same list also sorts purely by name
(the renderer puts program.fm first) and counts FolderSync's read-only copies (host.fm, library files) against the 16
files.

**Scenario.** A folder holding CIRCUIT's three files (program.fm with `#include "screamer.cir"` and `Screamer amp;`,
screamer.cir, panel.gui with knob drive / tone / level / volume). gui_sandbox {action: open, from: file, path:
&lt;folder>}: 'Dials: 1, 1 from the program's declarations' and 'there is no parameter drive' (tone, level); the same
files from the running instance give 4 dials and the 'Circuit dials (...)' line.

**How it was checked.** Confirmed by trace, and by a harness of `lang::circuitDials`: one binding with the `.cir`
among the files, none without it, and none when a helper file sorts before `program.fm`.

**Suggested fix.** Let readPath take isProgramFile's kinds (Sources.h), put program.fm first as
Source/render/Main.cpp:247-250 does, and leave out FolderSync's read-only copies as the renderer does (a copied
library .cir is no program file in the plugin); then bring SandboxProtocol.h:49, kSandboxNotes, the refusals' words
and FP_MCP_VERSION along.

### UI-24. A window opened while the chat is folded asks for a frame on every tick

**Severity:** medium. **Where:** `Source/editor/Panel.cpp:7726`. **Evidence:** reproduced by a run.

**What is wrong.** Panel::tick() calls layoutChat() whenever the message box's wanted height differs from composerH_,
but layoutChat() sets composerH_ only on its open path; folded (or with no chat at all, Role::code) it returns early
after `++layoutRevision_; dirty_ = true;`. A Panel constructed with the chat folded (the session keeps
EditorPort::chatOpen) or as the code window keeps composerH_ at its initial 0, so the test is true on every tick for
as long as the chat stays folded: needsDraw() is true after each tick (CLAUDE.md's 'Unchanged frames' rule: an idle
editor records and draws nothing) and a11yRevision(), which mixes layoutRevision_, changes two steps a tick.

**Scenario.** Fold the chat, close the plugin's editor and open it again (or open the code window) and leave it idle:
every calm frame, 12 a second, runs layoutChat() and records the whole panel; needsDraw() is true after every tick.

**How it was checked.** Reproduced in a headless editor: a Panel made while the chat was folded asked for 24 frames in
two idle seconds at 12 ticks a second; the Panel whose chat was folded by a click asked for 2.

**Suggested fix.** Guard the tick's test with chatOpen_ && hasChat(), or set composerH_ = in.desiredHeight(...) before
layoutChat()'s early return, so a folded chat's box asks for no layout.

## Low

Each line: the finding as the second pass worded it, where it is, and the likely fix. All were confirmed by an
independent trace of the code unless the line says more.

### Turn loop and conversation state

- **TURN-8** (`Source/plugin/ChatAgent.cpp:3067`). A stop during the wait before a retry puts the broken attempt's
  partial text into the history, though the chat took it back. *Fix:* When the stop comes after a broken attempt was
  taken back, treat it as an attempt with no answer ("(stopped by the user)"), or do not take the text back.
- **TURN-9** (`Source/plugin/ChatAgent.cpp:3228`). The first check nudge says the turn ends on an answer without a
  tool call, but such an answer is measured and nudged once more. *Fix:* Word the first nudge as what happens ("the
  plugin asks once more if it still fails"), or end the turn when a nudge's reply makes no tool call.
- **TURN-10** (`Source/plugin/ChatAgent.cpp:1078`). ask_user's limits count bytes where its schema and refusals say
  characters: labels and questions within the stated limit are refused in any script but ASCII (the cut inside a
  character is harmless on screen). *Fix:* Count and cut by characters (or cut back to a character's start, as
  ChatChats.cpp:61-62 does), or say bytes in the schema.
- **TURN-11** (`Source/plugin/ChatAgent.cpp:2881`). A stop before the turn's first request (during /v1/models or the
  planner) ends the turn with no "Stopped." note, and a plan that was streaming is erased. *Fix:* After the loop, when
  cancel_ is set and the turn added no note, add "Stopped."; keep a plan's text that had begun (marked as stopped)
  instead of erasing its entry.
- **TURN-12** (`Source/plugin/ChatAgent.cpp:3402`). runTurn (and mcpCall) clear turnThread_ after they released busy_:
  a call or turn that starts in between loses its turn thread. *Fix:* Store the empty turnThread_ inside the same lock
  that clears busy_, in runTurn's end and mcpCall's end, as the early exits do.
- **TURN-15** (`Source/editor/Panel.cpp:6113`). The message box is cleared whatever send() did: a message send()
  refuses because the agent became busy since the panel's last look is lost. Also found as UI-20. *Fix:* Have send()
  return whether it took the message (or have sendChat look at chatState() after it) and clear the box only then.

### API client and other providers

- **API-5** (`Source/plugin/Providers.cpp:80`). canonical() takes http://010.x.y.z as a 10.x local address (atoi reads
  010 as ten) while libcurl sends it to 8.x.y.z: plain HTTP can leave the local network through a zero-padded address.
  *Evidence:* Reproduced with a harness of the real source lines and libcurl's URL parser (parse only); what NSURL and
  WinINet do with a zero-padded octet was not settled. *Fix:* In localHost(): refuse an IPv4 part with a leading zero
  (size > 1 and front == '0'); for a bracketed host require the closing ']' and only hex digits, ':' and '.' inside;
  refuse '\\' and '%' in the authority and a non-numeric port. Rows in plugin.providers URL for each.
- **API-8** (`Source/plugin/ChatProviderTurn.cpp:456`). An OpenAI-compatible model's cut-off reply is said to have
  reached "64K tokens, its thinking included": a figure the request never carried and the server never promised.
  *Fix:* Name no number for a provider whose request carried none (maxTokens 0 from startTurn for an OpenAI-compatible
  model, and wording that says the server cut the reply, pointing at its context setting), or send max_tokens and name
  that.
- **API-10** (`Source/plugin/ChatProviderTurn.cpp:384`). A server whose models list came back empty is never listed
  again: "listed none" stays after the user pulls a model, until the plugin is reopened, the provider changed, or a
  model typed by hand. *Fix:* Leave otherFetched_ false when the list is empty (set it back before the return at line
  384), so the next turn or the next Save asks again.
- **API-11** (`Source/plugin/Providers.cpp:1400`). Error texts are cut by bytes (std::string::substr), so a non-ASCII
  error body can end inside a UTF-8 character. *Fix:* Cut with lang::headOf(text, n) (fminus/Lang.h, FMinus-free) at
  the five sites.

### Key and chat stores, chat ownership

- **STORE-2** (`Source/plugin/ChatStore.cpp:297`). A key or chat file is not renamed over: juce::File::moveFileTo
  unlinks the target first, so between two system calls there is no file. *Fix:* Replace in one step:
  juce::File::replaceFileIn (rename(2) on POSIX, ReplaceFile on Windows: juce_File.cpp:323-335) or mcp::replacePath's
  rename/MoveFileExW, and correct the comment.
- **STORE-4** (`Source/plugin/ChatStore.cpp:631`). Linux, a session bus and libsecret but no Secret Service behind it:
  removing a key reports 'could not be removed' every time (and shows it kept for 3 s), although the key's file was
  deleted and the keyring never held it. *Fix:* When has() cannot ask (-1) and clear() failed, do not count the store
  as holding the key unless there is evidence it did (the store answered 1 for this item before, or no file existed,
  i.e. the key was read from the store); report the store's error as a note rather than a failed removal once the file
  is deleted.
- **STORE-5** (`Source/plugin/ChatProjects.cpp:104`). Claude Desktop's projects open clears the checks of the chat it
  is about to leave (the owner switch is deferred to the call's end, the clear is not). *Fix:* Clear the checks only
  when the conversation is carried (not call.desktop), or, for a pending open, leave them to the owner switch at the
  call's end and word Desktop's result from the chat that will show.
- **STORE-6** (`Source/plugin/ChatChats.cpp:353`). A pending owner change (Claude Desktop's projects open) is applied
  at the call's end without looking again: after busy_ is cleared, with no turn stopped, and over a project the user
  opened meanwhile. *Fix:* Apply the pending change before busy_ is cleared (still inside the call), drop it in
  setOwner whenever an owner change completes or the owner asked equals chatOwner_, and at apply time ask the
  processor for the owner now (syncChatOwner) instead of trusting the parked one.
- **STORE-7** (`Source/plugin/ChatChats.cpp:157`). keep() takes the redraw counter for 'the chat changed': a model or
  thinking pill (any touch()) rewrites the whole chat and stamps it 'last changed now', moving an old chat to the top
  and making it the project's newest. *Fix:* Give the conversation its own change counter (bumped where messages_,
  entries_, checks_, asking_ or the title change) for keepLocked's test, or compare a hash of saveState(true) and the
  meta's fields with what was last written; keep the old updatedMs when only the file is rewritten.

### File tools, projects, pictures

- **FILES-4** (`Source/plugin/ProcessorSaves.cpp:324`). projects save_version on a project that holds 100 versions
  drops its oldest, while the result, the description, the prompt and the README say every version is kept. *Fix:* Say
  in the result which version went when the cap dropped one (the store knows), and word the description, prompt and
  README as 'the newest 100 versions are kept'.
- **FILES-5** (`Source/plugin/ChatImport.cpp:544`). write_file, rename_file, delete_file and add_library_file store
  back the whole Draft they read (chatSetDraft): typing the editor pushed between the read and the write is reverted
  in every other file. *Fix:* Write through a setter that takes only the files the tool changed (or a name change) and
  leaves the others as they are now, under programMutex_, as chatSetDraftIf does.
- **FILES-7** (`Source/plugin/ChatFiles.cpp:853`). read_file's range and find never show a line longer than 12,000
  characters, and their 'cut here' advice names the same line again. *Evidence:* Reproduced: `read_file {from: 2,
  lines: 1}` on a 20,000-character line ended "(cut here: read from 2 for the rest)". *Fix:* When one line alone is
  past the room left with nothing shown yet, show it cut where a character starts, with its length, and say that a
  whole read (no from, lines or find) gives it all.
- **FILES-8** (`Source/dsp/Regex.h:9`). Regex.h's example of a pattern refused as too large, (?:()()()a){1000},
  compiles (1001 instructions). *Evidence:* Reproduced with a harness of `Regex.cpp`: the pattern compiles to 1,001
  instructions. *Fix:* Change the header's example to one the cap refuses (nineteen or more empty groups a copy, or
  the probe's nested pattern), or word it as 'more than about 20,000 calls in all'.
- **FILES-10** (`Source/plugin/ProcessorSaves.cpp:440`). projects: apply_preset is documented to refuse over unsaved
  files and samples but checks dials alone; a factory copy saved by its factory name with no project open is called a
  copy of the factory's "". *Fix:* (1) Say in the description, ChatAgent.h and CLAUDE.md that apply_preset refuses
  over moved dials (files are untouched). (2) Use `isFactoryName(name) ? name : st.name` at line 317.

### Measuring and debugging tools

- **TOOLS-3** (`Source/plugin/ChatAgent.cpp:4252`). view_sound's render takes no cancel: Stop, an MCP cancel and a
  join wait for it to end (a picture's render: at most 10 s held + 10 s tail of sound). *Fix:* Make the Session in the
  tool (as panelState does) and call session.setCancel(renderCancel("Drawing the sound")) before
  measure::renderSound(session, vr), returning cancelWords() when Sound::cancelled; or give measure::view a Cancel
  parameter.
- **TOOLS-4** (`Source/plugin/ChatAgent.cpp:4972`). tune_dials tunes the editor's files but applies by name to the
  running program's dials, and says "The dials are set to the best values" even when none (or only some) matched.
  *Fix:* Count the dials actually set; for a tuned name the running program lacks (the draft is not what plays) say so
  and tell Claude to run_program and set_dial the values, instead of the unconditional sentence.
- **TOOLS-5** (`Source/plugin/ChatTrace.cpp:178`). trace's export picks a free stem, then writes with a replacing
  rename (two instances exporting the same stem at once replace each other's files), and a later file's failure leaves
  the earlier files unnamed. *Fix:* Claim the name exclusively (create the target with O_EXCL / CREATE_NEW, or link(2)
  / renameat2(RENAME_NOREPLACE) / MoveFileEx without REPLACE_EXISTING, retrying with the next number), and on a later
  file's failure name the files already written in the error.
- **TOOLS-6** (`Source/plugin/ChatScenario.cpp:217`). Render settings of the wrong JSON type are dropped without a
  word (renderOf's notes, velocity, hold_seconds, tail_seconds, bpm, wheels, dials; set_checks' index), and compare
  ignores render settings it does not take. *Fix:* Refuse a present-but-mistyped value in renderOf and checkAt as
  Reader::num does (or accept a numeric string), and have compare refuse the render settings it does not take, as it
  already refuses window.
- **TOOLS-8** (`Source/plugin/ChatFunction.cpp:349`). run_function's fresh instance says "the dials as they are" but
  sets only the main module's params: a circuit's pot/switch dials and Shared's dial params stay at their defaults.
  *Fix:* After the setValue loop, build the controls' plain values and call patch.setSharedDials(plain) and
  patch.setCircuits(plain, 0) (as many times as a switch's ramp takes), or say which dials a fresh instance leaves at
  their defaults.
- **TOOLS-10** (`Source/plugin/ChatCompare.cpp:128`). "playing" in compare, trace and view_panel's at compiles the
  playing program's files with the self-test store's answers, not with the configuration its netlists play with
  (CompileStatus::circuits). *Fix:* Let chatPlaying (or a sibling) return the status's CompileStatus::circuits and
  pass them as the Session's pins for a 'playing' side (a pins member on CompareSide, q.pins in trace, panelState's
  Session), as run_function does.

### Claude Desktop link, live tools, journal

- **LINK-2** (`Source/plugin/ChatAgent.cpp:4949`). mcpCancelled_ is one slot: a cancel of another call (a listen's)
  right after the running tune_dials' cancel makes the tune apply its best values after all. *Fix:* Remember the
  running call's cancel under mutex_ (a bool set in mcpCancel's `mcpRunning_ == call` branch and from `early` at
  mcpCall's start, cleared when a call starts) and have tune_dials read that instead of comparing mcpCancelled_.
- **LINK-3** (`Source/plugin/ProcessorAssets.cpp:127`). Claude Desktop's own sample add, delete and rename are
  journalled as "the plugin's own chat" and head its own result as a change. *Fix:* Gate the three journal notes on
  !chat_.mcpWorking(), as chatSetDraft, chatRun and chatSetDial are (pass no author to putAsset/replaceAssets then).
- **LINK-4** (`Source/mcp/McpProtocol.h:548`). claim() probes every lower-numbered socket with a blocking connect() in
  the plugin's constructor: on Linux the tenth new instance hangs while another instance's listener is open but not
  accepted on (its host stopped or stuck). *Evidence:* Reproduced on Linux with a harness around the real
  `mcp::claim()`; the macOS half was not run. *Fix:* Make the probe non-blocking: O_NONBLOCK before connect(), then a
  short poll() for writability; treat EAGAIN/EINPROGRESS/a timeout as 'an instance is there' (skip the number), and
  only ECONNREFUSED as stale.
- **LINK-5** (`Source/mcp/McpProtocol.h:555`). claim() tests a socket, then unlinks and binds in separate steps: a
  second starter inside the first one's bind()-to-listen() gap takes the same number, and the first one's exit then
  unlinks the second's socket. Also found as SERVER-5. *Evidence:* Reproduced with a harness around the real
  `mcp::claim()`: in 109 of 4,000 rounds of eight claims at once, two claimers took one number. *Fix:* Serialise
  test-unlink-bind (an flock'd lock file in the folder, as FolderSync's .fmsync-lock), or bind and listen under a
  private temporary name and link() it to &lt;n>.sock (link fails when the name exists); have the destructor unlink
  path_ only while its inode is still the one it bound.
- **LINK-6** (`Source/plugin/Journal.cpp:323`). The MCP hint says "The output guard acted" for a program that stopped
  (every row whose author is the plugin is counted as a guard alert). *Fix:* Count alerts by the change's key
  ("alert:") rather than by its author, or give the stop a wording of its own in mcpHint.
- **LINK-7** (`Source/plugin/Live.cpp:618`). get_status's description promises each changed file as a diff with
  `since`; from the plugin's own chat that mode gives nothing and says nothing. *Fix:* Either word the chat's copy of
  the description for what it gets, or, with mcp false and files "changed", add one line ("read_file shows a file;
  files \"all\" all of them").
- **LINK-8** (`Source/plugin/Journal.cpp:91`). File edits merge across a RUN: typing after a run is folded into the
  'edited' line that stands before the run's line in get_status since. *Fix:* Treat a run's row (and the other
  setsDials rows that replace what plays) as a barrier for file keys too, so the edit after it starts a new line.
- **LINK-9** (`Source/plugin/ChatFiles.cpp:995`). The file-versions record is one for every caller: edit_file's
  `expected` refusal tells one Claude that the other's write was "one of your own calls". Also found as FILES-6.
  *Fix:* Keep who gave each version (the chat, or MCP) in FileVersion and count a version as the caller's own only
  when the same side gave it; or say 'a call here' rather than 'your own' when the sides differ.

### MCP server and GUI sandbox

- **SERVER-1** (`Source/mcp/Main.cpp:1864`). gui_sandbox apply with `instance` set: a notifications/cancelled that
  lands while the instances' info is gathered does not stop it; the plugin's .gui is still written and run, and
  nothing is answered. *Fix:* Put `if (!stillOpen(id)) return;` at the top of withInstance's gather continuation
  (Main.cpp:1133), which covers callTool, open from instance and apply alike (or at least at the top of applyTo).
- **SERVER-2** (`Source/mcp/Main.cpp:941`). `instance` in a number's form ("3", "Synth 3", "Bass (Synth 3)") that no
  open instance has falls through to the word match and reaches the one instance whose track, project or name holds
  that digit. *Evidence:* Reproduced with a harness around the real matcher in `McpTarget.h`. *Fix:* Before the word
  match, refuse a wanted whose form is a number's name ('&lt;kind> N' or '... (&lt;Kind> N)', and probably a bare N)
  when no open instance has number N; 'moog synth 2' (rename.reached_as_listed) still passes because instance 2 is
  open.
- **SERVER-3** (`Source/mcp/Main.cpp:490`). A JSON-RPC batch (a line that is a JSON array) is answered with one -32700
  'Parse error', id null, though the server accepts protocol 2025-03-26, which requires receiving batches. *Fix:*
  Either handle an array line by running handle() on each element and writing the answers as one array (only needed
  for 2025-03-26), or stop echoing 2025-03-26 (answer 2025-06-18); answer -32600 for JSON that is not an object.
- **SERVER-4** (`Source/mcp/Main.cpp:1891`). apply: when write_file refuses, the reason quoted is the 'changed since
  the last MCP call' hint, not the refusal; and the text says 'ran it, so the user sees it' when the run failed.
  *Fix:* For the failure, skip the hint block (take the text after hintOf(w), or join every text block); build the
  'and ran it, so the user sees it' sentence only when the run compiled, else say the .gui was written but the run
  failed.
- **SERVER-8** (`Source/mcp/Main.cpp:1896`). gui_sandbox apply has no deadline of its own: its asks' deadlines add up
  (5 s + 20 s + 200 s + 200 s), so it can stay unanswered past Claude Desktop's 240 s when the instance's one worker
  is queued up. *Fix:* Take one deadline at the client's request (Clock::now() + callMs_ in sandboxTool/applyTo) and
  give each ask of the chain what is left of it, answering 'Still running' when it passes.
- **SERVER-9** (`Source/sandbox/Sandbox.cpp:319`). open from file checks stat's size, then reads to end of file with
  no S_ISREG check and no bound: a FIFO named \*.gui/.fm/.sc/.sh stalls the helper until the 30 s deadline, a link to
  a device is read without limit. *Evidence:* Reproduced with a harness of the same system calls: a link to a device
  was read at about 23 GB in 30 s. *Fix:* Refuse what is not a regular file (stat's S_ISREG, also for the single .gui
  path) and read at most kMaxFileBytes + 1 bytes from the opened stream (readIntoMemoryBlock(mb, kMaxFileBytes + 1)),
  refusing when more came.
- **SERVER-10** (`Source/mcp/Main.cpp:917`). Instance matching folds case and cuts words per ASCII byte: a track or
  project named in a non-Latin script is reached only by its exact bytes, never 'in part' or 'case aside'. *Fix:*
  Lower and split by code point: juce::String::toLowerCase() for the exact pass and
  juce::CharacterFunctions::isLetterOrDigit over the string's characters in words().

### Chat panel

- **UI-5** (`Source/editor/ChatView.cpp:598`). Copy (an answer's, a code block's) copies the ASCII transliteration the
  panel draws, not the text the entry holds. *Fix:* Build the Copy payloads from e.text (the code block's from the raw
  lines between its fences), keeping asciiText for what is drawn.
- **UI-6** (`Source/editor/Panel.cpp:3237`). The Send button is disabled for a message of pictures alone, which Return
  sends. *Fix:* Add `|| !chat_.attached.empty()` to the send item's enabled test, as sendChat's own condition has it.
- **UI-7** (`Source/editor/ChatView.cpp:372`). Inline `code` inside small text (opened thinking, > quotes, option
  descriptions, check details, notes) is drawn at 12 px in cells laid out for 10.5 px, so it runs over the next word.
  *Fix:* Give inline code a style of the base's size (a small code role, 10.5 px, for the small bases), or advance the
  column by each piece's own width.
- **UI-8** (`Source/editor/ChatView.cpp:672`). The user's own message is drawn with every asterisk and backtick
  replaced by a space (and the gap closed up). *Fix:* Have flow() take the paragraph as plain text for the user's
  bubble (skip inlinePieces) and leave the characters in.
- **UI-9** (`Source/editor/Panel.cpp:8013`). The chat header's title (another provider's name) is not fitted: a server
  name of 32 characters or more at the least chat width leaves the chats pill no width, and from 35 it is drawn over
  the header's buttons. *Fix:* Fit the title (fitText) to the room left of the header's buttons less kChatsPillLeast,
  and place the pill after the fitted width (chatsPillX from the same string).
- **UI-10** (`Source/editor/ChatItems.cpp:250`). Attached pictures that do not fit the message box's one row are not
  drawn and (but for a 4 px invisible strip) cannot be removed with the pointer. *Fix:* Wrap the tiles onto a second
  row (stripHeight then follows the rows), or shrink them to fit six in the box's width.
- **UI-11** (`Source/editor/ChatView.cpp:1779`). The conversation scrolls by the wheel alone: no key and no
  accessibility action reaches an earlier message, and blocks out of view are left out of the accessibility tree.
  *Fix:* Make the view a Tab stop that takes Page Up/Down, Home and End (and the arrows) as scroll keys, and either
  list every block or scroll a block into view on a11yAction focus.
- **UI-12** (`Source/editor/Panel.cpp:6478`). Key dialog: no key reaches the Provider pill or Remove key (Return and
  Space never open the dropdown), and the wheel scrolls a .gui interface behind it. *Fix:* Give the dialog a focus
  order (fields, Provider, Remove key, Cancel, Save) with Return or Space pressing the focused control, as
  SettingsCard does; return true from Panel::wheel while keyOpen_.
- **UI-14** (`Source/editor/ChatView.cpp:258`). A tool row is drawn as failed when its text merely contains 'error',
  and a run that stopped is drawn as a normal run. *Fix:* Carry the outcome with the entry (a failed flag on
  ChatEntry, set where isError is) instead of reading it out of the words.
- **UI-15** (`Source/editor/Panel.cpp:4219`). Dragging an audio or project file over the editor lights the chat's
  picture overlay ('Drop pictures here for Claude' / 'Drop to add for Claude'). *Fix:* Set dropActive_ in
  filesDragEnter only when one of the paths isPictureName (keep the paths' kinds for the overlay's words).
- **UI-16** (`Source/editor/ChatView.cpp:989`). Opened thinking rows, folded plans and an open question's picks are
  kept by entry index and carry over to another chat with as many or more entries. *Fix:* Give ChatState the shown
  chat's id (or bump a 'chat changed' count) and clear the three maps in show() when it changes.
- **UI-17** (`Source/editor/Panel.cpp:6570`). Key dialog: a click in a field never places the caret or drops the
  selection (the rename box ignores it), a server address or model past 73 characters shows only its head, and the
  104-character address refusal is cut by four characters. *Fix:* On a press in a field set its caret from the
  pointer's x (kBody's advance) and clear allSelected, in the rename box too; draw a long field scrolled to its caret;
  let the refusal wrap to a second line.
- **UI-21** (`Source/editor/ChatView.cpp:1340`). An answered question also marks options whose label is the start or
  the end of the chosen label around a ', '. *Fix:* Keep the picked indices with the answered entry (or match the
  answer against the labels as whole joined items, longest first), not substrings.
- **UI-22** (`Source/editor/Panel.h:60`). Panel.h, ChatView.h and gui.providers' header describe chat behaviour the
  code does not have. *Fix:* Correct the four comments (drop 'asking first' and 'and its transcript', point to
  Items.h, say combo box) and add the missing controls to Panel.h's chat paragraph.
- **UI-23** (`Source/editor/Panel.cpp:8676`). A refused Save in the key dialog is silent to a screen reader: the
  reason is only the Key field's help text (also for a refused address or model), and it is never announced. *Fix:*
  When keyWhy_ is set, call host_->announce(keyWhy_) and put the reason on the field it is about (or the dialog's
  description); no revision change is needed.

### Descriptions, annotations, strings

- **DOCS-1** (`Source/plugin/ChatAgent.cpp:3708`). write_file is sent to MCP with destructiveHint false, though it
  replaces a file's whole text and its `path` replaces a sample of the same name, which nothing can undo. *Fix:* Mark
  write_file destructive (and gui_sandbox, whose apply overwrites the .gui), or refuse a `path` whose name is taken
  unless asked; say 'added, or replacing the sample of that name' in the schema; update mcp.cpp's server.annotations
  row.
- **DOCS-3** (`Source/plugin/ChatAgent.cpp:2308`). Literal 'this Mac', 'the user's Mac' and '/Users/...' in tool
  schemas and results, chat notes, provider errors and two provider names on every platform, where kThisMachine exists
  for it. Also found as TOOLS-7, FILES-9, UI-13. *Evidence:* Traced, and visible in the Linux build's probe pictures:
  the key dialog's title "Ollama (this Mac)" above "A model server on this computer", the refusal "http:// is only for
  a server on this Mac or the local network", and the footer "API KEY CMD-V PASTES IT" beside the field's own "(Ctrl-V
  pastes it)". *Fix:* Route each through kThisMachine (the presets' names built at run time, or 'on this computer' off
  macOS), share ChatCompare's kWholePath with ChatImport, and reword the case refusal for portability; update the
  probes' expected strings.
- **DOCS-4** (`Source/plugin/ChatAgent.cpp:95`). The fallbacks and fast-mode model tables predate Claude Fable 5.2 /
  Mythos 5.2: no `fallbacks` and no Fast pill on them, the built-in list still offers Fable 5.1, and README names Fast
  for Opus 5.5 alone. Also found as API-7. *Fix:* Add claude-fable-5-2 and claude-mythos-5-2 to takesDefaultFallbacks
  and to takesFast (no beta), put Fable 5.2 in builtInModels, drop claude-opus-4-8 from takesFast after 2026-11-30,
  and say in the README which models show Fast.
- **DOCS-5** (`Source/plugin/ChatAgent.cpp:2494`). set_checks' item schema does not declare the per-check dials_from
  and snapshot that the handler reads and the listing emits, and requires a label the handler defaults. *Fix:* Declare
  dials_from (the enum, no description: dropRepeated keeps the call's) and snapshot (an object) on the item, and drop
  label from `required`; or say in the description that a listed item's dials_from and snapshot are taken as given.
- **DOCS-6** (`Source/plugin/ChatAgent.cpp:4236`). view_sound's close-up is taken only when both from_seconds and
  to_seconds are numbers (and to is after from): one alone is ignored without a word. *Fix:* With from_seconds alone,
  take to = from + a few cycles (or the 2 s cap); refuse to_seconds alone or to &lt;= from in words; or say 'give
  both' in the schema.

## Plausible, not proven

The mechanism is in the code, but whether it can be reached, or what the peer does with it, could not be settled
without a live service or another platform.

- **TURN-6** (`Source/plugin/ChatAgent.cpp:3093`). A fallback turn goes back without its fallback block, where the
  reference says to keep the block exactly where it appeared. Also found as API-4. *Not settled:* Whether the API in
  fact refuses this exact shape cannot be settled without calling it: the quoted sentence speaks of "thinking blocks
  from both sides of the boundary", and the code omits the thinking before the boundary within that message, so it
  turns on whether earlier turns' thinking counts as the other side (the reference does not say), and on whether the
  account and model enforce the check or drop the blocks instead. Not checked against the live API (no live call was
  made). *Fix:* Keep the fallback block where it came (drop only what the reference lists before it), and change
  fallback.history to expect it; decide the citations of text before the boundary the same way as the stop path.
- **API-6** (`Source/plugin/Providers.cpp:1239`). OpenAI-compatible stream: whole parallel calls that share one index
  are merged; the first runs with its own arguments and the second vanishes without an error. *Not settled:* Which
  servers a user would run today still send several whole calls under one index (older Ollama builds and Gemini's
  OpenAI-compatible endpoint are the candidates; no live server was tried). *Fix:* Start a new call when a chunk for
  an index carries a non-empty id other than the one that call began with (keep the id per Call), or a name once the
  call already has a name and arguments.
- **TURN-7** (`Source/plugin/ChatAgent.cpp:1264`). worker_ is joined and started with no lock: a host that sets the
  state off the message thread while the user sends races on it, and a turn may start between the join and the chat's
  replacement. *Not settled:* Which thread a host calls setStateInformation on. The VST3 rule and common practice are
  the host's UI thread, the Standalone loads its state on the message thread, and AU hosts use the main thread in
  practice, but none of that is guaranteed and JUCE does not move the call. With a host that keeps the rule this
  cannot happen; with one that does not, it also needs the user to send (or press New chat) within the same
  milliseconds. *Fix:* Do the chat's part of a state load on the message thread (hand loadChat to poll(), as
  setCurrentProgram's work is), or guard worker_ and the switch with one mutex that send() and answer() take too.
- **TURN-13** (`Source/plugin/ChatAgent.cpp:3199`). After a pause_turn, a stop or a failed request leaves the paused
  reply's pending server_tool_use in the middle of the history, the stop path adding a second assistant message after
  it. *Not settled:* Whether the API refuses a server_tool_use without its result once it is not the last block (after
  a merged second assistant message, or before a user message), or lets it pass. The reference does not say and no
  call could be made; also whether every paused reply ends in such a block. *Fix:* When a turn ends after a paused
  reply, take the reply's server_tool_use blocks that have no result out of it (and join the stop's text to that
  message instead of adding a second assistant message).
- **SERVER-7** (`Source/mcp/McpProtocol.h:213`). Windows socketPair() takes accept()'s first connection on its
  loopback port without checking that the peer is its own connecting socket. *Not settled:* Whether Windows, after
  listen(l, 1) with one foreign connection waiting in the accept queue, still completes the server's own connect()
  (hijack: the foreign socket becomes the pair's end) or refuses it (the pair fails; a failed start only). Not tested:
  no Windows build was made. *Fix:* After accept(), compare getpeername(d) with getsockname(c) and close and accept
  again on a mismatch (or bind an AF_UNIX pair in the private folder, which this code already uses on Windows 10
  1803+).

## Considered and refuted, and what held

Refuted:

- **STORE-8** (`Source/plugin/Processor.cpp:2075`). A state from another instance that kept no chats here gives the
  loading instance a new id, which leaves its own earlier no-project chats unlisted. This is the documented rule
  (CLAUDE.md, "One instance, one id"), not a contradiction of it; keeping the processor's own id would be a design
  change.
- Parts of confirmed findings that did not hold: Gemini's two lines in the key dialog fit (UI-17; only the 104-character
  address refusal is cut, by four characters); the key dialog taking Cmd-Q is the stated design of every card (UI-12);
  FunkGui does re-sync a field's help text without a revision change (UI-23, whose other half stands: the refusal is
  never announced); a merge in set_checks does not "replace all the checks" (DOCS-5); whether Windows also delivers a
  stream in 8 KB pieces was not settled from source (API-3 is confirmed for macOS).

Checked and found sound:

- **The stores.** The MAC is verified before anything is decrypted, with a constant-time compare; each file has a fresh
  IV and salt; a provider's key is bound to its server's origin in the file and in the keyring; owners and ids are
  validated before they become paths; `storeMutex_` and `mutex_` are taken in one order; the session and both exports
  hold no word of a chat and no project id; on Linux `has()` never unlocks the keyring and a test's folder never
  reaches it.
- **The live tools.** get_status, listen, and capture's status and stop read only atomics, seqlock copies and locked
  getters: no data race was found on the live path itself.
- **The network rules.** The transport and the provider layer were read in full and no finding was raised against the
  no-redirect rule or against a key's headers going to its own origin alone; the one gap found in the `http://` rule is
  API-5.
- **Regex and TextDiff.** `dsp/Regex.cpp` agreed with Python's `re` on 204,000 fuzzed pattern and text pairs (no
  mismatch but Python's own `\B` on empty text), and `dsp/TextDiff.cpp` passed 400,000 random cases.
- **The texts.** About 50 checkable claims of the system prompt, 25 of the MCP guide and sandbox notes, and every number
  CLAUDE.md gives for the chat (the request cap, the nudge cap, the compaction trigger, the picture limits, the MCP
  deadline and picture size, the render caps, the search budget, the journal and token counts, and the rest) match the
  code. The API version, the beta names and the web tool versions agree with the Claude API reference.

## Noticed outside the chat

Not in scope, seen while looking at the Linux build's pictures:

- **Uppercase shortcut hints are never converted.** `shortcutText` replaces "Cmd-" only, and the editor's status and
  footer hints are written in capitals, so the Windows and Linux builds show "CMD-RETURN RUNS THE PROGRAM", "CMD-F
  FINDS", "CMD-SHIFT-D SWITCHES THE VIEW", "CMD-Z" and the like (`Source/editor/CodeAssist.cpp:2881-2894`,
  `Source/editor/Panel.cpp:1894-1933`, `2934`, `5693`, `Source/editor/SchematicView.cpp:3524-3598`), and "ON THIS MAC"
  (`Panel.cpp:2946`). The program card's status reads "Edited: cmd-return runs it" in `gui.chat.small.png`. The chat's
  own two lines of this kind are in DOCS-3.
- **The caret covers a placeholder's first letter.** In an empty, focused field (the message box, the key field, an
  empty file's tab) the caret is drawn over the left edge of the placeholder's first glyph ("Answer above" in
  `gui.chat.ask.png`). Cosmetic, and likely FunkGui's TextEditor rather than this repository.

## Method

1. **Sweep.** Nine reviewers, each given one area and told to read its files in full, to state a concrete failing
   scenario for every finding, and to try to refute it first. Eight ran on the most capable model (the logic-heavy
   areas); the ninth, a mechanical cross-check of what the prompt, the tool descriptions, the guide, the README and
   CLAUDE.md say against what the code does, ran on a cheaper one. For anything about Anthropic's API the reviewers
   checked the Claude API reference rather than memory.
2. **Second pass.** Eight verifiers, each with a fresh context and the brief to refute: they re-read the code
   themselves (JUCE 8.0.4's and FunkGui v0.26.1's sources included, where a finding rested on them), looked for the
   guard, the caller or the probe that would make the scenario impossible, and re-ran or rebuilt the reviewers'
   harnesses. They lowered the severity of sixteen findings, raised two, refuted one whole and parts of five.
3. **Runs.** A Linux build of the synth's probe executable. The nine existing chat probes passed as a baseline
   (`plugin.chat`, `plugin.chats`, `plugin.chatstore`, `plugin.providers`, `plugin.live`, `plugin.import`,
   `plugin.saves`, `gui.chat`, `gui.providers`). Two temporary probes then drove the findings marked "reproduced by a
   run" with the scripted API (`FakeClaude.h`) and a headless editor; their rows are in the appendix. The pictures the
   GUI probes write (`gui.chat*.png`, `gui.providers*.png`) were read for visual defects.
4. **The build.** Ubuntu 24.04, clang 20 and lld, CMake 4.1, `-DFP_WERROR=OFF`, FMinus's prebuilt LLVM 23.1.3. Two
   stand-ins were needed, neither touching the chat's code: the system's bison 3.8.2 in place of the prebuilt one
   (which wants a libtextstyle this container lacks), and PFFFT cloned at its pinned commit `aa16fd3` in place of the
   pinned tarball (the session could not download the archive). Only the synth's probes were built; the chat's code is
   the same in both products.

## Limits

- **Platforms.** Only Linux was run. What is macOS's or Windows' alone (API-1 and API-3's transports, CNG, the Windows
  socket shims, Core Image) was judged from source.
- **No live service.** No request was made to Anthropic or to any other provider. Where a finding turns on what a
  service does with a request (TURN-2's 400, TURN-6, TURN-13, API-6), the entry says what that rests on.
- **Scope.** The DSP analysis under the measuring tools (`Source/dsp/Measure.cpp` and its neighbours) was out of
  scope, except where a front end misuses it.
- **Not reached.** The recorder's internals (`Source/plugin/Capture.cpp`) and the body of `Source/dsp/Debug.cpp`;
  `cir::apply` and `parseOp` behind edit_circuit; `assetfiles::read` behind write_file's `path`; the explorer's CLAUDE
  DESKTOP section and the name card; `Tools/probes/gui/menus.cpp`'s chat rows; the README's estimate of the short
  prompt's size (it needs a count of the built prompt).
- **A high confirmation rate is not a clean bill.** The second pass could only judge what the first pass raised. The
  areas above that were not reached, and the platforms that were not run, may hold more.

## Appendix: reproduction rows

Rows that state the correct behaviour, so each fails today. They use the helpers of the probe named (`fake::Script`,
`fake::Response`, `fake::Sse`, `messagesOf`, `arr`, `str`, `clickInput`, `type`, `clickAt`, `chat`, `a11yTitled`).

`Tools/probes/plugin/chat.cpp`, beside COMPACT and STOP (TURN-1, TURN-2):

```cpp
// TURN-1: Stop while a compaction block is arriving. `before` is the history's size before this message.
fake::Response stall;
stall.stream = fake::Sse().start().str()
               + "event: content_block_start\r\ndata: {\"type\":\"content_block_start\",\"index\":0,"
                 "\"content_block\":{\"type\":\"compaction\"}}\r\n\r\n";
stall.stall = true;
script->add(stall);
agent.send("third question");
while (script->streaming.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
agent.stop();
agent.waitIdle();
const auto after = arr(claude::parseJson(agent.saveState()).getProperty("messages", {}));
P.ge("compact.stop_keeps_history", static_cast<double>(after.size()), before + 1.0);   // today: 1
bool empty = false;
for (const auto& m : after)
    for (const auto& b : arr(m.getProperty("content", {})))
        empty = empty || (str(b, "type") == "compaction" && str(b, "content").empty());
P.eq("compact.no_block_without_summary", empty ? 1 : 0, 0);                             // today: 1

// TURN-2: a reply with no content block.
script->add(fake::Response::ok(fake::Sse().start().end()));
agent.send("anything");
agent.waitIdle();
script->add(fake::Response::ok(fake::Sse().start().text("Hello.").end()));
agent.send("again");
agent.waitIdle();
bool hole = false;
const auto sent = messagesOf(script->body(script->postCount() - 1));
for (int i = 0; i + 1 < sent.size(); ++i)
    hole = hole || arr(sent[i].getProperty("content", {})).isEmpty();
P.eq("empty.no_empty_message_sent", hole ? 1 : 0, 0);                                   // today: 1
```

`Tools/probes/plugin/providers.cpp`, TRANSPORT (API-1). It passes on Linux (libcurl) and fails on macOS and Windows:

```cpp
// The listener and readRequest()/sendText() are TRANSPORT's own.
const auto r = claude::httpsTransport()->post("http://127.0.0.1:" + std::to_string(port),
                                              "/v1beta/models/x:streamGenerateContent?alt=sse", "{\"probe\":true}",
                                              { { "content-type", "application/json" } },
                                              [](std::string_view) { return true; }, cancel);
P.eq("transport.query_kept", got.rfind("POST /v1beta/models/x:streamGenerateContent?alt=sse ", 0) == 0 ? 1 : 0, 1);
P.eq("transport.body_alone", got.substr(got.find("\r\n\r\n") + 4) == "{\"probe\":true}" ? 1 : 0, 1);
```

`Tools/probes/plugin/chat.cpp`, PICTURES (FILES-3). `huge` is a PNG of 16,000 x 16,000 one-bit pixels of one colour,
about 31 KB (a signature, IHDR, one IDAT of zlib over 16,000 rows of 2,001 zero bytes, IEND):

```cpp
struct rusage a{}, b{};
getrusage(RUSAGE_SELF, &a);
agent.attach({ huge });
while (agent.chatState().attaching > 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
getrusage(RUSAGE_SELF, &b);
P.le("pictures.huge_memory_mb", static_cast<double>(b.ru_maxrss - a.ru_maxrss) / 1024.0, 256.0);   // today: 1706
```

`Tools/probes/gui/chat.cpp`, beside STOP, COLLAPSE and RESIZE (UI-1, UI-2, UI-24, UI-3). `stalled()` is STOP's reply
that hangs until the agent cancels:

```cpp
// UI-1: a double-click on Send.
script->add(stalled());
clickInput(host, panel);
type(host, "go");
const funkgui::Point s = panel.centreOfSend();
host.doubleClick(s.x, s.y);
host.tick(4);
P.eq("send.double_click_sends_once", chat(processor).busy ? 1 : 0, 1);                  // today: 0, and no note

// UI-2: where the keys go after a click on Send.
script->add(stalled());
clickInput(host, panel);
type(host, "go");
clickAt(host, panel.centreOfSend());
host.tick(4);
P.eq("send.click_keeps_caret", panel.chatInput().focused() ? 1 : 0, 1);                 // today: 0
host.keys("space");
host.tick(4);
P.eq("send.space_does_not_stop", chat(processor).busy ? 1 : 0, 1);                      // today: 0

// UI-24: with the chat folded, a second Panel on the same processor, idle.
pg::ui::Panel second(processor.ports(), processor.editorPort());
funkgui::HeadlessHost h2(second, 0, 2.0f);
h2.settle();
h2.tick(30, 1.0f / 12.0f);
h2.draw();
int asked = 0;
for (int k = 0; k < 24; ++k)
{
    h2.tick(1, 1.0f / 12.0f);
    if (second.needsDraw()) { ++asked; h2.draw(); }
}
P.le("draw.folded_quiet", static_cast<double>(asked), 3.0);                             // today: 24

// UI-3: the least window (host.resize(500, 300)), a turn streaming.
const auto web = a11yTitled(host, "Web search"), send = a11yTitled(host, "Send or stop");
P.le("resize.least_bar_clear", web->bounds.x + web->bounds.w, send->bounds.x);          // today: 951 > 936
const bool was = chat(processor).web;
clickAt(host, panel.centreOfSend());
host.tick(4);
P.eq("resize.least_stop_stops", chat(processor).busy ? 0 : 1, 1);                       // today: 0
P.eq("resize.least_stop_leaves_web", chat(processor).web == was ? 1 : 0, 1);            // today: 0
```
