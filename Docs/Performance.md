# Performance

## Performance tab

Window → **Cursor Unreal** → **Performance**. Two tables:

### Recent tool calls

| Column | Meaning |
|--------|---------|
| Time | Local clock of the call |
| Tool | Registry name |
| Caller | `native`, `cursor-sdk`, or `mcp` |
| Risk | ReadOnly / Mutating / Dangerous |
| Status | ok / error |
| Queue | Time waiting for the game thread |
| Approval | Time in the approval gate (0 when auto-allowed) |
| Run | Tool body duration |
| Transport | Client→editor delay when the caller stamped `ClientSentUnixMs` |

### Recent agent turns

| Column | Meaning |
|--------|---------|
| Model | Model id for the turn |
| Outcome | finished / cancelled / error |
| First | Time to first streamed token |
| Total | End-to-end turn wall time |
| ModelMs / ToolMs | Time spent in the model vs tool execution |
| Trips / Calls | Round-trips and tool invocations |
| In / Cached / Out | Token counts when the provider reports them |

**Export CSV** writes under the perf log directory (`Saved/CursorUnreal/` by default). **Clear** empties the in-memory tables; disk history is separate unless you delete those files.

## Tool timing

Every `FCursorToolRegistry::Invoke` records duration in the perf log. Slow Mutating tools usually dominate Run; high Queue with a low Run means the editor was busy or throttled.

## MCP latency

Unfocused-editor figures (override off / on) are recorded in [`measurements/phase1-mcp-latency.md`](measurements/phase1-mcp-latency.md). Focused-editor measurement is deferred until a visible foreground editor session is available.

## Background throttle

When the editor is unfocused, a settings override can reduce tick throttling so MCP / agent tool calls stay responsive. Prefer measuring both modes before changing defaults.

## Content estimates

The optimisation pack reports StaticMesh LOD0 triangle sums and optional last-frame RHI counters — useful for content triage, not a substitute for GPU profilers (`stat gpu`, Insights).
