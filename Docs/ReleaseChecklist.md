# Release checklist

Use before handing someone a packaged build. Plan phases 0–5 cover this; nothing further is gated on MCP IDE wiring.

## Build

- [ ] `./Scripts/Build-Plugin.ps1` succeeds on the target engine
- [ ] `./Scripts/Run-Tests.ps1 -Filter CursorUnreal` — all pass
- [ ] `./Scripts/Build-Plugin.ps1 -Mode Package`

## Package contents

Under `Packaged/<EngineVersion>/CursorUnreal/`:

- [ ] `CursorUnreal.uplugin`, `Binaries/`, `Source/`
- [ ] `Skills/` (nine built-in skill folders)
- [ ] `Bridge/` with `bridge.mjs`, `package.json`, `lib/`, `test/` — **no** `node_modules/`
- [ ] After install into a project: `cd Bridge && npm install` if using the Cursor provider

## Docs

- [ ] README quick start still matches Setup tab labels
- [ ] [Compatibility](Compatibility.md) lists the engine version you actually packaged

## Optional verification (skip freely)

- Live Anthropic turn with a real key
- Live Cursor provider turn with a real key
- Cursor IDE MCP list/call via `mcp.json`
- Focused-editor MCP latency figure
