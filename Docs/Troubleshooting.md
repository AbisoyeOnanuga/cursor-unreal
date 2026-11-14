# Troubleshooting

## Plugin does not load

- Confirm the host project Engine Association matches an installed engine.
- Build with `./Scripts/Build-Plugin.ps1` and check the Output Log for module load errors.
- If Niagara is disabled in the project, expect `CursorUnrealToolsNiagara` to fail loading unless you remove that module from the `.uplugin`.

## Tools missing

- Project Settings → Cursor Unreal → disabled tool packs.
- `source_*` tools are NativeAgentOnly (hidden from MCP / Cursor SDK).
- Refresh the Skills / Setup tabs after rebuilding.

## Bridge / Cursor provider

- Run `npm install` in `Plugins/CursorUnreal/Bridge`.
- Check Bridge status on the Setup tab (Node path, SDK version, last error).
- Keys must be stored in Setup; an empty key yields an immediate init failure.

## MCP

- Server must be running in an open editor.
- Token in `mcp.json` must match the token shown in Setup.
- Loopback only — remote hosts will not connect.

## Automation tests

```powershell
./Scripts/Run-Tests.ps1 -Filter CursorUnreal
./Scripts/Run-Tests.ps1 -Filter CursorUnreal.Tools
```

Exit code **255** with `NoSuchTest` means the filter matched nothing (often a closed editor or stale binaries), not a failed assertion.
