<#
.SYNOPSIS
    Renders each tab of the Cursor Unreal panel to a PNG, for the documentation.

.DESCRIPTION
    Starts the editor off screen in demo mode (-CursorUnrealDemo), where the chat plays back a
    scripted reply. No API key is used and nothing is sent to any service. The token counts and
    model name in the images are invented by that script; the tool calls and their timings are real.

    The MCP server is switched off for this run so it cannot collide with an editor you have open.

.EXAMPLE
    ./Scripts/Take-Screenshots.ps1
    ./Scripts/Take-Screenshots.ps1 -OutDir C:\Temp\shots
#>
[CmdletBinding()]
param(
    [string]$OutDir = "",
    [string]$EngineRoot = "",
    [int]$TimeoutSeconds = 600
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Project = Join-Path $RepoRoot "CursorUnrealDev.uproject"
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot "Docs\images" }

if (-not $EngineRoot) {
    $Association = (Get-Content $Project -Raw | ConvertFrom-Json).EngineAssociation
    $EngineRoot = (Get-ItemProperty -Path "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$Association" -ErrorAction SilentlyContinue).InstalledDirectory
}
$Editor = Join-Path $EngineRoot "Engine\Binaries\Win64\UnrealEditor.exe"
if (-not (Test-Path $Editor)) { throw "UnrealEditor.exe not found under '$EngineRoot'. Pass -EngineRoot." }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$Log = Join-Path $RepoRoot "Saved\Logs\Screenshots.log"
New-Item -ItemType Directory -Force -Path (Split-Path $Log) | Out-Null
Remove-Item $Log -ErrorAction SilentlyContinue
$Started = Get-Date

$EditorArgs = @(
    "`"$Project`"",
    "-CursorUnrealDemo",
    "-CursorUnrealShots=`"$OutDir`"",
    "-ini:EditorPerProjectUserSettings:[/Script/CursorUnrealCore.CursorUnrealSettings]:bEnableMcpServer=False",
    "-RenderOffScreen", "-unattended", "-nosplash", "-nopause", "-nosound",
    "-abslog=`"$Log`""
)
$Process = Start-Process -FilePath $Editor -ArgumentList $EditorArgs -PassThru -WindowStyle Hidden
if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
    $Process.Kill()
    throw "Timed out after $TimeoutSeconds seconds. Log: $Log"
}

Get-Content $Log | Select-String -Pattern "Screenshots:" | ForEach-Object { Write-Host "  $($_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '')" }
$Written = Get-ChildItem $OutDir -Filter "panel-*.png" | Where-Object { $_.LastWriteTime -ge $Started }
if ($Written.Count -eq 0) { throw "No images were written. Log: $Log" }
$Written | ForEach-Object { Write-Host ("  {0}  {1:N0} bytes" -f $_.Name, $_.Length) }
