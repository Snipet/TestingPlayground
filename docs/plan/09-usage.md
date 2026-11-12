# 09. AI usage and cost

Planning-document item: "Improve AI chat feature by adding model and token usage metric on a dedicated page. There
should be bar charts and ideally real price metrics." Size M to L. Decision D10. Needs 07 (the Settings card's
page); the prices' refresh needs 08.

## 1. What the user gets

A Usage page: how many tokens each model used, day by day, as bars; what that cost, exact where the provider says
and an estimate at list prices elsewhere, labelled as such with the price table's date.

## 2. What exists

- `claude::Usage` (`Source/plugin/ClaudeApi.h`): `input`, `output`, `cacheRead`, `cacheWrite`, `compactions`,
  `webSearches`, `webFetches`, `speed`. `MessageBuilder` fills it from `message_start` and `message_delta`, sums a
  compaction's iterations, and keeps the model that served the request (`model()`), which nothing reads.
- **Only one string survives**, `ChatAgent::usage_`: "Last reply: N tokens in (M cached), K out...", rebuilt
  after every request of a turn, shown under the message box (`ChatState::usage`). Nothing is summed.
- **Lost today:** a retried or broken request's tokens (a new builder each attempt); the fast-mode fallback's and
  the web tools' second requests; the planner's (only a string, "planner X in, Y out"); the cache write's split
  by lifetime (`usage.cache_creation`, not parsed; the system prompt is cached for an hour, which costs twice
  the input price to write, not 1.25 times); which model served after a fallback.
- **Other providers** (`Source/plugin/Providers.cpp`, `providers::Stream`): OpenAI-compatible servers give
  prompt, completion and cached tokens; Gemini's `usageMetadata` likewise. **OpenRouter's `usage.cost` is read
  past**, and so is the `pricing` object of its models list.
- `ChatModel` (`ChatPort.h`) has no price. Anthropic's models list (`GET /v1/models`) returns none.
- The folder both products share (`Funk/Playground/`) holds the keys, `chat.json` (choices only) and the sealed
  chats. `chat.json` is written with no lock between processes: the last writer wins.
- The editor has bar-like drawing in `looks::meter` and `drawSpectrum`, nothing for a chart.

## 3. Design

### 3.1 Counting every request

`Source/plugin/Usage.h/.cpp`, JUCE-free (a ledger and its arithmetic):

```cpp
namespace usage
{
    struct Key    { std::string day;        // the user's local day, "2026-11-12"
                    std::string provider;   // "anthropic", "openrouter", "openai", "gemini", "local", "other"
                    std::string model;      // the model that served the request
                    bool fast = false; };
    struct Counts { int64_t requests = 0, input = 0, output = 0, cacheRead = 0, cacheWrite5m = 0, cacheWrite1h = 0,
                    webSearches = 0, webFetches = 0;
                    int64_t reportedMicroUsd = 0; int64_t reportedRequests = 0; };   // a provider's own figure
    void add(const Key&, const Counts&);     // any thread
    Snapshot snapshot(Range);                // any thread: a copy
}
```

- `ChatAgent` adds **once for every request that reached the API and reported usage**, whatever became of it: a
  turn's rounds, each retry and broken stream (what its `message_start` and last `message_delta` said), the
  fast-mode and web-tool second requests, the planner's, a provider's work-around request.
- The model is the one the response names (`MessageBuilder::model()`), so a fallback is counted under the model
  that ran. `fast` is `usage.speed`, what was actually used.
- Anthropic: parse `usage.cache_creation.ephemeral_5m_input_tokens` and `ephemeral_1h_input_tokens`; a server
  without them counts all of `cache_creation_input_tokens` as 5m. Check the API reference before touching the
  parsing (CLAUDE.md, "The API").
- OpenRouter: read `usage.cost` into `reportedMicroUsd`. Its documentation asks for `"usage": {"include": true}`
  in the request for it; whether today's servers send it without is one of the things no probe can check live
  (README's list for the user to try).
- The provider is its kind, never a server's address (a private server's name is the user's own business); a
  server on this machine or the local network is "local".
- The footer string stays as it is (probes hold its words), plus, when the turn's cost is known, ", $0.12".

### 3.2 Keeping it

- `Funk/Playground/usage.json`: `{"format": "funk-playground-usage", "version": 1, "days": [{"day", "provider",
  "model", "fast", ...counts}]}`. Counts and names only: **no word of a chat, no key, no address, no project.**
  Plain, 0600 as the folder's files are.
- Written at a turn's end and when the agent goes, on the agent's worker, never under its `mutex_`: read the
  file, add this process's counts since its last write, write beside and rename, all under the folder's `.lock`
  (the factory state's way). Two instances, two products and two hosts therefore add up instead of overwriting.
- Kept: 400 days by day; older days folded into one row a month.
- **Reset** on the page, asked first, empties it.

### 3.3 Prices (D10)

`usage::Prices`: for a provider and a model, USD a million tokens for input, output, cache read, the two cache
writes, and fast input and output; a web search's price; its date.

- **Built in:** a table compiled into the plugin (`Source/plugin/UsagePrices.cpp`, one row a model, its date in
  one constant). The rows at the time of this plan, from the API reference as cached on 2026-11-03; check each
  against the pricing page when implementing:

  | Model | In | Out | Cache read | Write 5m | Write 1h | Fast in / out |
  |---|---|---|---|---|---|---|
  | claude-fable-5-2 | 8 | 40 | 0.20 | 10 | 16 | 16 / 80 |
  | claude-fable-5-1 | 10 | 50 | 0.25 | 12.50 | 20 | |
  | claude-fable-5 | 10 | 50 | 1.00 | 12.50 | 20 | |
  | claude-opus-5-5 | 4 | 20 | 0.20 | 5 | 8 | 8 / 40 |
  | claude-opus-5, claude-opus-4-8 | 5 | 25 | 0.50 | 6.25 | 10 | 10 / 50 |
  | claude-opus-4-7, claude-opus-4-6 | 5 | 25 | 0.50 | 6.25 | 10 | |
  | claude-sonnet-5-5 | 2 | 10 | 0.10 | 2.50 | 4 | |
  | claude-sonnet-5 | 2 | 10 | 0.20 | 2.50 | 4 | |
  | claude-sonnet-4-6 | 3 | 15 | 0.30 | 3.75 | 6 | |
  | claude-haiku-5-5 (prompts to 100K tokens) | 0.10 | 0.50 | 0.01 | 0.125 | 0.20 | |
  | claude-haiku-4-5 | 1 | 5 | 0.10 | 1.25 | 2 | |

  Cache writes are 1.25 times the input price for five minutes and twice for an hour; a cache read is a tenth of
  it unless the row says otherwise. A web search is $10 a thousand. Haiku 5.5 costs five times as much for a
  prompt past 100K tokens: the estimate uses the lower rate and the footnote says so.
- **Refreshed:** the website's `/app/v1/release.json` carries the same table (`prices`, package 08, 2.2). When the
  update check is on, each check hands it over; a table with a later `asOf` than the built-in one is used and
  kept in `app.json`. It may hold other providers' models too (OpenAI's, Gemini's), which is the only way those
  get a price. With the check off, the built-in table is all there is.
- **OpenRouter's list:** its models' `pricing` (prompt and completion, USD a token) is read when the list is
  (`providers::modelsOf`) and kept as that model's price, for requests whose cost was not reported.
- **A cost** for a row: its reported figure where every request reported one; else the table's price times its
  counts, marked an estimate; else none (tokens only, "no price known").

### 3.4 The page

Settings, Usage (`Source/editor/UsagePage.h/.cpp`, a page as 07's table has them), through
`ChatPort::usage(Range)`:

- A range: 7 days, 30 days, 12 months, All. A switch: Tokens or Cost.
- **The chart:** a bar a day (a month in the 12-month view), stacked by model, the largest models a colour each
  (the theme's accent first, then a fixed set of the theme's inks and tones that hold in both themes), the rest
  "Other". The y axis in round steps ("2M", "$5"). The pointer over a bar shows that day's rows under the chart;
  the arrow keys move along the bars; each bar is an accessibility item with its day and total.
- **The table:** a row a model: its provider, requests, tokens in, cached, out, the cost and a bar of its share;
  a total row.
- **The footnote:** "Costs marked ~ are estimates at list prices as of 2026-11-03. OpenRouter's are as reported.
  Your provider's invoice is the record." And **Reset...**.
- Empty: "No requests yet." with the chart's frame.
- The drawing is rounded rects on the canvas (`ChatStyle.h`'s palette), a small `ui::BarChart` in the same files;
  it draws only when its snapshot or the pointer's bar changes (`drawKey`).

## 4. Work

1. `Usage` (the ledger, the file, the arithmetic), `UsagePrices`, and `ChatAgent` adding at every request;
   `Providers` reading OpenRouter's cost and prices; the footer's cost.
2. `ChatPort::usage`, the page, the chart.
3. The refresh from the website's JSON (after package 08).

## 5. Tests

- New probe `plugin.usage`: the arithmetic for each model in the table (input, output, both cache writes, a
  cache read, fast, a web search); an unknown model has tokens and no price; a reported cost wins; a row partly
  reported is an estimate; two ledgers writing the same file add up; 400 days, then months; a refreshed table
  with a later date replaces the built-in one and an earlier one does not; the file holds no text but names and
  numbers (a scan of it after a chat with known words).
- `plugin.chat`: over the scripted API (`FakeClaude.h` already emits usage; it gains `cache_creation`'s split),
  RETRY, BROKEN, FALLBACK, PLANNER, WEB and COMPACT each add what they should: a retried request counts twice,
  a fallback under the model that served, the planner under its model. The footer's words unchanged.
- `plugin.providers`: OPENROUTER (the reported cost, the list's prices), OPENAI and GEMINI (tokens, no price
  until the table has one), a local server ("local").
- New probe `gui.usage`: the page empty, a week, a year; Tokens and Cost; the stacks' order; the readout under
  the pointer and by the keys; the footnote's date; Reset asked first; both themes, the least window. PNGs.

## 6. Docs

README, "The chat": the Usage page, what is counted, what the costs are and are not. CHANGELOG: Added. CLAUDE.md:
rows for `Usage`, `UsagePrices`, `UsagePage`; in "The chats: on this Mac only", `usage.json` in the list of what
the folder holds and why it carries no chat (counts and model names only); the rule that the ledger is added to
for every request, retries included; the price table's date moved with every release (the release recipe).

## 7. Risks

- **An estimate is not an invoice.** Batch discounts, a provider's own rounding, a price change between the
  table's date and the request: the footnote says so, and the page never calls an estimate a cost.
- **The table goes stale** in a build with the update check off. The date is always shown.
- **A model the table does not know** (new, or another provider's) shows tokens with no price, never a guess.
