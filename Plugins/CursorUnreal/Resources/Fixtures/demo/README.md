# Demo script

These three files are the replies the chat panel plays back when the editor is started with
`-CursorUnrealDemo`. They are hand-written in the shape of Anthropic Messages API event streams;
none of them is a recording of a real model.

One demo turn uses all three, in order:

| File | What the scripted model does |
| --- | --- |
| `turn-1.sse` | Thinks, says one line, calls `editor_get_context` |
| `turn-2.sse` | Calls `list_skills` |
| `turn-3.sse` | Writes the closing answer |

The script repeats for every message you send. Nothing is sent to any service. The two tool
calls are real: they run in the editor and are both read-only.

The token counts in these files are invented, so the Performance tab shows made-up numbers in
demo mode.
