# Architecture

CursorUnreal is an editor plugin: an in-editor agent panel, a shared tool registry, a local MCP server, and an optional Node bridge for the Cursor provider.

## Modules

| Module | Role |
|--------|------|
| `CursorUnrealCore` | Tool registry, approval gate, settings, secrets, skill library, JSON helpers, perf log |
| `CursorUnrealTools` | Editor, assets, Blueprint, C++, materials, animation, audio, optimisation packs |
| `CursorUnrealToolsNiagara` | Niagara pack (separate so projects can drop Niagara) |
| `CursorUnrealAgent` | `IAgentBackend`, Anthropic streaming backend, Cursor bridge backend, sessions |
| `CursorUnrealMCP` | Loopback MCP server (bearer token + Origin check) |
| `CursorUnrealEditor` | Slate panel: Chat, Activity, Performance, Skills, Setup |

## Call paths

All three callers hit `FCursorToolRegistry::Invoke` on the game thread:

1. **Native** — in-editor Anthropic (or mock) agent
2. **cursor-sdk** — Node `@cursor/sdk` bridge over NDJSON stdio
3. **mcp** — Cursor IDE / other MCP clients

Approval, undo wrapping, timing and result truncation are identical across callers.

## Skills

Built-in `SKILL.md` folders live under `Plugins/CursorUnreal/Skills/`. The same format works in Cursor IDE when installed into the project `.cursor/skills` layer.

## Bridge

`Plugins/CursorUnreal/Bridge/` is a Node sidecar. The editor starts it when Provider is Cursor; the API key is passed on stdin during init, not as a CLI flag.
