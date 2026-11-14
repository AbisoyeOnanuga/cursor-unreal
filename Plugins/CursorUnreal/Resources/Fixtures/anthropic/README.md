# Anthropic stream fixtures

These files are **written by hand** from the event shapes in Anthropic's streaming
documentation (API version `2026-10-21`). They are not recordings of real API traffic: no
request was made to produce them, and the ids, signatures and token counts in them are made up.

They exist so the Anthropic backend can be tested, and the chat panel demonstrated, with no API
key, no network and no cost. `FAgentFixtureTransport` replays them in place of the real
connection.

What that means for trust:

- They prove the backend handles the documented event sequence correctly, including awkward
  delivery (split mid-character, split mid-line-ending) and failures (overload, truncation,
  refusal, dropped connection).
- They do not prove the live API sends exactly these shapes. Where the documentation does not
  show a shape in full, the fixture is a best reading of it. `refusal.sse` is the main case: the
  `stop_details` object is documented as carrying the policy category, and the field name
  `category` used here comes from that description rather than from a captured response.

| File | What it plays back |
| --- | --- |
| `text-only.sse` | A plain reply, with a `ping` and text containing multi-byte characters. |
| `tool-use.sse` | Thinking, a sentence of text, then one tool call (`__agent_echo`). |
| `two-tools.sse` | Two tool calls in one reply. |
| `after-tool.sse` | The closing reply after tool results were sent back. |
| `overloaded-midstream.sse` | Some text, then an `overloaded_error` event inside a 200 stream. |
| `max-tokens-mid-tool.sse` | A reply cut off by `max_tokens` half-way through a tool call's arguments. |
| `refusal.sse` | A reply that stops with `refusal`. |
| `cut-off.sse` | A stream that ends with no `message_stop`, as a dropped connection would leave it. |
| `models.json` | A `GET /v1/models` reply with three models of differing capabilities. |
| `error-401.json`, `error-429.json`, `error-400.json` | Error bodies in the documented shape. |

`__agent_echo` is a tool the automation tests register for themselves; it does not exist in a
normal editor session.
