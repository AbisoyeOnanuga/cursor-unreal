<#
.SYNOPSIS
    Runs the plugin's automation tests in a headless editor and prints a pass/fail summary.

.EXAMPLE
    ./Scripts/Run-Tests.ps1                       # everything under CursorUnreal
    ./Scripts/Run-Tests.ps1 -Filter CursorUnreal.Core
    ./Scripts/Run-Tests.ps1 -Filter CursorUnreal.Tools.Screenshot -Rendering

.PARAMETER Rendering
    Starts the editor with a renderer (off screen, no window) instead of without one. Needs a graphics card and
    takes longer to start. The tests that take pictures of the level only do so in this mode, and they keep the
    pictures under Saved/CursorUnreal/TestShots so you can look at them.
#>
[CmdletBinding()]
param(
    [string]$Filter = "CursorUnreal",
    [string]$EngineRoot = "",
    [switch]$Rendering,
    [int]$TimeoutSeconds = 900
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Project = Join-Path $RepoRoot "CursorUnrealDev.uproject"

if (-not $EngineRoot) {
    $Association = (Get-Content $Project -Raw | ConvertFrom-Json).EngineAssociation
    $EngineRoot = (Get-ItemProperty -Path "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$Association" -ErrorAction SilentlyContinue).InstalledDirectory
}
$Editor = Join-Path $EngineRoot "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (-not (Test-Path $Editor)) { throw "UnrealEditor-Cmd.exe not found under '$EngineRoot'. Pass -EngineRoot." }

$Log = Join-Path $RepoRoot "Saved\Logs\AutomationTests.log"
New-Item -ItemType Directory -Force -Path (Split-Path $Log) | Out-Null
Remove-Item $Log -ErrorAction SilentlyContinue

$EditorArgs = @(
    "`"$Project`"",
    "-ExecCmds=`"Automation RunTests $Filter; Quit`"",
    "-unattended", "-nosplash", "-nopause", "-nosound",
    "-abslog=`"$Log`""
)
if ($Rendering) { $EditorArgs += @("-RenderOffscreen", "-CursorUnrealKeepShots") } else { $EditorArgs += "-nullrhi" }
$Process = Start-Process -FilePath $Editor -ArgumentList $EditorArgs -PassThru -WindowStyle Hidden
if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
    $Process.Kill()
    throw "Timed out after $TimeoutSeconds seconds. Log: $Log"
}

$Lines = Get-Content $Log
$Results = $Lines | Select-String -Pattern 'Test Completed\. Result=\{(\w+)\} Name=\{([^}]*)\} Path=\{([^}]*)\}'
$Passed = 0; $Failed = 0
foreach ($Match in $Results) {
    $Result = $Match.Matches[0].Groups[1].Value
    $Path = $Match.Matches[0].Groups[3].Value
    if ($Result -eq "Success") { $Passed++; Write-Host "  PASS  $Path" }
    else { $Failed++; Write-Host "  FAIL  $Path ($Result)" -ForegroundColor Red }
}

if ($Failed -gt 0 -or $Results.Count -eq 0) {
    # Show what the failing tests said.
    $Lines | Select-String -Pattern 'LogAutomationController: (Error|Warning)|Error: .*CursorUnreal|No automation tests matched|Fatal error|Assertion failed' |
        Select-Object -First 60 | ForEach-Object { Write-Host "    $($_.Line)" }
}

Write-Host "$Passed passed, $Failed failed. Log: $Log"
$Shots = Join-Path $RepoRoot "Saved\CursorUnreal\TestShots"
if ($Rendering -and (Test-Path $Shots)) { Write-Host "Pictures taken by the tests: $Shots" }
if ($Results.Count -eq 0) { throw "No tests ran for filter '$Filter'." }
if ($Failed -gt 0) { exit 1 }
