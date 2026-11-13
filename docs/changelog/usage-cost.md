### The chat counts its tokens and says what a turn cost

**Added**
- The line under the message box adds what the turn has cost so far, once every request in it has a price: "~$0.07"
  is an estimate at list prices (Anthropic's from a table built into the plugin, dated 2026-11-12; an OpenRouter
  model's from its models list), "$0.04" a provider's own figure (OpenRouter's). A model with no known price shows its
  tokens and no cost.
- Every request the chat sends is counted, by day and by the model that answered, in `usage.json` beside the chats:
  retries, the planner's and fast mode's and web search's second tries included, and the tokens written to the cache
  for five minutes and for an hour apart. It keeps counts and model names only, never a word of a chat, a key or a
  server's address. A page to see it comes later.
