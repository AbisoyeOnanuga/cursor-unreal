# Compatibility matrix

Built and exercised on the versions below. Other 5.x releases may work; treat them as unverified until packaged.

| Engine | Host project association | Editor build | Plugin package (`Build-Plugin.ps1 -Mode Package`) | Notes |
|--------|--------------------------|--------------|---------------------------------------------------|--------|
| **5.6.1** | yes (`CursorUnrealDev.uproject`) | yes (2026-11-14) | yes → `Packaged/5.6.1/CursorUnreal/` (includes `Skills/`, `Bridge/` sans `node_modules`) | Primary target. MSVC 14.50. Automation: 33/33 `CursorUnreal` tests pass. |
| 5.5.x | not associated | not run | not run | HTTP stream delegate shim exists (`CURSORUNREAL_HAS_HTTP_STREAM_DELEGATE_V2`). |
| 5.7.x | not associated | not run | not run | Pass `-EngineRoot` to `Build-Plugin.ps1` when installed. |

## Version shims

Engine differences are gated in `CursorUnrealCompat.h` via `CURSORUNREAL_UE_AT_LEAST` / feature macros. Prefer adding a shim there instead of scattering `ENGINE_MINOR_VERSION` checks.

## Optional plugins

| Plugin | Required? |
|--------|-----------|
| Niagara | Enabled in `.uplugin`; Niagara tools live in `CursorUnrealToolsNiagara` |
| PythonScriptPlugin | Enabled for `python_exec` |
| EditorScriptingUtilities | Enabled for editor scripting helpers |

## Node bridge

Requires a local Node.js install and `npm install` inside `Plugins/CursorUnreal/Bridge`. Bridge tests use a stand-in SDK; live Cursor API calls are not part of CI.
