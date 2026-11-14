# CursorUnreal

AI agent tooling for the Unreal Editor (UE 5.6+): an in-editor chat panel, a shared tool registry (Blueprint, C++, materials, Niagara, animation, audio, optimisation), shipped skills, and a local MCP server for Cursor.

## Quick start

1. Open `CursorUnrealDev.uproject` in **UE 5.6.1** (or build the plugin into your own project).
2. Build: `./Scripts/Build-Plugin.ps1`
3. Window → **Cursor Unreal**. Set Provider (Anthropic or Cursor) and store an API key on the Setup tab.
4. Optional: `npm install` in `Plugins/CursorUnreal/Bridge` for the Cursor provider.
5. Optional: install built-in skills to the project from the Skills tab so Cursor IDE can read them.

## Docs

| Doc | Topic |
|-----|--------|
| [Architecture](Docs/Architecture.md) | Modules and call paths |
| [Providers](Docs/Providers.md) | Anthropic, Cursor bridge, MCP |
| [Writing a tool](Docs/WritingATool.md) | Pack authoring |
| [Writing a skill](Docs/WritingASkill.md) | `SKILL.md` format and layers |
| [Security](Docs/Security.md) | Secrets, risk tiers, boundaries |
| [Performance](Docs/Performance.md) | Timing and MCP latency notes |
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

- Primary target: **UE 5.6.1**
- Live Anthropic / Cursor API calls and Cursor IDE MCP wiring are optional verification steps (not required to build or run automation fixtures).
