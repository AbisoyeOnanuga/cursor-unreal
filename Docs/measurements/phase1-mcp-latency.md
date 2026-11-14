# Phase 1 measurements: MCP call latency and the background throttle

Recorded 14 November 2026 on one machine. These are the first measured numbers for the speed
claims in the plan, which until now were reasoned from engine source.

## Result

Round trip for one `tools/call` over the loopback MCP server, 40 sequential calls on one
keep-alive connection, calling a cheap read-only tool (`editor_log_tail`, one line).

| Editor state | Median | p95 | Min | Max | Time inside the tool (median) |
|---|---|---|---|---|---|
| Rendering, not in the foreground, override **off** | 339.35 ms | 345.83 ms | 328.55 ms | 352.81 ms | 0.85 ms |
| Rendering, not in the foreground, override **on** | 17.78 ms | 30.70 ms | 14.33 ms | 32.09 ms | 0.77 ms |
| Headless (`-unattended -nullrhi`), 50 calls | 17.54 ms | 30.32 ms | 13.84 ms | 35.25 ms | 0.65 ms |

"Override" is the plugin setting **Stay Responsive In Background**
(`bStayResponsiveInBackground`), which registers a `ShouldDisableCPUThrottlingDelegates` entry.

## What this shows

- With the override off, every call waits for the editor's next tick, and an editor that is not
  in the foreground ticks at 3 fps. 339 ms per call matches that 333 ms tick.
- With the override on, calls take about one frame. The tool itself takes under a millisecond in
  every row, so the rest is waiting for the tick; transport is a small part.
- The override is about 19 times faster for this workload (339.35 / 17.78).

## What this does not show

- **A focused editor was not measured.** Making the editor the foreground window means opening
  it on the desktop, which was not done for these runs. With the override on, the engine takes
  the same no-throttle path as when focused, so the focused figure is expected to be close to
  17.78 ms. That is an expectation, not a measurement.
- **The first call after the editor has been idle is still slow with the override on:**
  340.31 ms (342.84 ms with it off). The editor is throttled until it sees a request from an
  agent, and it only sees the request on its next throttled tick. After that, calls are fast for
  as long as the agent keeps working, and for 30 seconds after its last request.
- **Small sample.** One run per row, one machine, a blank project. Treat the medians as
  indicative. A heavy project has a longer frame, and calls will take about one of its frames.
- **Headless is not a stand-in for unfocused.** A headless editor never applies the throttle, so
  its row only shows that the transport works.
- Only the MCP path was measured. The in-editor agent and the Cursor bridge do not exist yet.

## Machine

- AMD Ryzen 7 5800H (16 logical cores), 31 GB RAM, AMD Radeon integrated graphics, D3D12
- Windows 10.0.26300, Unreal Engine 5.6.1 (installed build), Node v24.18.0
- Project: `CursorUnrealDev` (blank host project), Development Editor build

## How to reproduce

Build, then start an editor that renders but has no window. Do not pass `-unattended`: it turns
the throttle off and hides the effect being measured.

```powershell
./Scripts/Build-Plugin.ps1

$Editor = "C:\Program Files (x86)\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
$Setting = "-ini:EditorPerProjectUserSettings:[/Script/CursorUnrealCore.CursorUnrealSettings]:bStayResponsiveInBackground=False"
& $Editor "$PWD\CursorUnrealDev.uproject" -RenderOffScreen -nosplash -nosound $Setting
```

Wait for the log line `AssetRegistryGather time` before measuring: the engine does not throttle
while it is still discovering assets, which would make the "off" run look fast. The editor log
also states which way the setting is:

```
LogCursorUnreal: Background throttle override installed. Stay responsive in background: off
```

Then, from another terminal:

```powershell
./Scripts/Run-McpProbe.ps1 -Calls 40 -Label "override off"
```

Repeat with `=True` for the "on" row.

## Security checks made over the same socket

The probe also confirmed, against the running server rather than in a unit test:

| Request | Answer |
|---|---|
| No token | 401 |
| Wrong token of the right length | 401 |
| Correct token with its last character removed | 401 |
| Correct token, `Host: evil.example` | 403 |
| Correct token, `Origin: https://evil.example` | 403 |
| `GET` with the correct token | 405 |
| `GET` with no token | 401 |
| A dangerous tool (`python_exec`) with no way to approve it | refused, did not run |

Separately, Windows reported the listening socket as `127.0.0.1:18732`, owned by the editor
process, in each run, and the token was searched for in every log these runs produced and was
not found in any of them.

Not verified here: the Cursor IDE as the client. The probe is a plain MCP client written for
this purpose. Whether Cursor accepts this server's plain-JSON responses (no event stream) is
still to be confirmed.
