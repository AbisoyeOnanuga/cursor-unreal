# Providers

## Anthropic (in-process)

- Provider: **Anthropic** in the Setup tab.
- Key stored via the Windows secret store (`ISecretStore`), not in ini plaintext.
- Streaming tool loop uses the Messages API; model list is fetched dynamically when possible.
- Fixture / mock backend backs automation tests without calling the live API.

## Cursor (Node bridge)

- Provider: **Cursor**.
- Requires Node.js and `Plugins/CursorUnreal/Bridge` dependencies (`npm install` in that folder).
- Editor launches `bridge.mjs`; API key is sent on the init message over stdin.
- Tools are exposed as `@cursor/sdk` `customTools` with the same registry names.
- Built-in Cursor file tools replace the NativeAgentOnly `source_*` tools for that provider.

## MCP (Cursor IDE)

- Local loopback server from `CursorUnrealMCP`.
- Bearer token + Origin checks; plain JSON tool results.
- Wire the server into `~/.cursor/mcp.json` when you want IDE verification (user-gated; see `Docs/measurements/phase1-mcp-latency.md`).
