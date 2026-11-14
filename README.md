# CursorUnreal

AI agent tooling for the Unreal Editor (UE 5.6+): an in-editor chat panel, a shared tool registry (Blueprint, C++, materials, Niagara, animation, audio, optimisation), shipped skills, and a local MCP server for Cursor.

## Quick start

1. Open `CursorUnrealDev.uproject` in **UE 5.6.1** (or build the plugin into your own project).
2. Build: `./Scripts/Build-Plugin.ps1`
3. Click **Agent** on the level editor toolbar, or Tools → **Cursor Unreal**. Set Provider (Anthropic or Cursor) and store an API key on the Setup tab.
4. Optional: `npm install` in `Plugins/CursorUnreal/Bridge` for the Cursor provider.
5. Optional: install built-in skills to the project from the Skills tab so Cursor IDE can read them.

## Docs

| Doc | Topic |
|-----|--------|
| [Getting started](Docs/GettingStarted.md) | Install, first turn, package check |
| [Release checklist](Docs/ReleaseChecklist.md) | Pre-handoff package verification |
| [Architecture](Docs/Architecture.md) | Modules and call paths |
| [Providers](Docs/Providers.md) | Anthropic, Cursor bridge, MCP |
| [Writing a tool](Docs/WritingATool.md) | Pack authoring + worked example |
| [Writing a skill](Docs/WritingASkill.md) | `SKILL.md` format, layers, worked example |
| [Security](Docs/Security.md) | Secrets, risk tiers, boundaries |
| [Performance](Docs/Performance.md) | Performance tab, timing, MCP latency |
| [Compatibility](Docs/Compatibility.md) | Engine matrix and shims |
| [Troubleshooting](Docs/Troubleshooting.md) | Common failures |

## Tests

```powershell
./Scripts/Run-Tests.ps1 -Filter CursorUnreal
```

## Package

```powershell
./Scripts/Build-Plugin.ps1 -Mode Package
```

Output lands under `Packaged/<EngineVersion>/CursorUnreal/`.

## Status

- Plan phases **0–5 complete** on **UE 5.6.1** (build, 33 automation tests, package with Skills + Bridge).
- Optional later (not blocking): live Anthropic/Cursor API smoke, Cursor IDE `mcp.json` wiring, focused-editor MCP latency.
