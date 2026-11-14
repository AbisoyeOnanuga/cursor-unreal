<#
.SYNOPSIS
    Builds the CursorUnreal plugin against any Unreal Engine 5.x install.

.DESCRIPTION
    Two modes:
      -Mode Editor   (default) Builds the dev host project's editor target. Fast, incremental,
                     and what you want while iterating.
      -Mode Package  Runs RunUAT BuildPlugin to produce a distributable copy of the plugin
                     under Packaged/<EngineVersion>/. Use this to check another engine version.

.EXAMPLE
    ./Scripts/Build-Plugin.ps1
    ./Scripts/Build-Plugin.ps1 -EngineRoot "C:\Program Files\Epic Games\UE_5.7" -Mode Package
#>
[CmdletBinding()]
param(
    [string]$EngineRoot = "",
    [ValidateSet("Editor", "Package")]
    [string]$Mode = "Editor",
    [ValidateSet("Development", "DebugGame")]
    [string]$Configuration = "Development"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Project = Join-Path $RepoRoot "CursorUnrealDev.uproject"
$Plugin = Join-Path $RepoRoot "Plugins\CursorUnreal\CursorUnreal.uplugin"

function Resolve-EngineRoot {
    param([string]$Requested)

    if ($Requested) {
        if (-not (Test-Path (Join-Path $Requested "Engine\Build\Build.version"))) {
            throw "No Unreal Engine found at '$Requested' (Engine\Build\Build.version is missing)."
        }
        return $Requested
    }

    # Use the engine the host project is associated with, read from the launcher's registry entries.
    $Association = (Get-Content $Project -Raw | ConvertFrom-Json).EngineAssociation
    $Key = "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$Association"
    $Installed = (Get-ItemProperty -Path $Key -ErrorAction SilentlyContinue).InstalledDirectory
    if ($Installed -and (Test-Path (Join-Path $Installed "Engine\Build\Build.version"))) {
        return $Installed
    }
    throw "Could not find Unreal Engine $Association. Pass -EngineRoot explicitly."
}

$EngineRoot = Resolve-EngineRoot $EngineRoot
$Version = Get-Content (Join-Path $EngineRoot "Engine\Build\Build.version") -Raw | ConvertFrom-Json
$VersionName = "{0}.{1}.{2}" -f $Version.MajorVersion, $Version.MinorVersion, $Version.PatchVersion
Write-Host "Engine: $EngineRoot ($VersionName)"

if ($Mode -eq "Editor") {
    $Build = Join-Path $EngineRoot "Engine\Build\BatchFiles\Build.bat"
    & $Build CursorUnrealDevEditor Win64 $Configuration "-Project=$Project" -WaitMutex -NoHotReloadFromIDE
}
else {
    $Uat = Join-Path $EngineRoot "Engine\Build\BatchFiles\RunUAT.bat"
    $Out = Join-Path $RepoRoot "Packaged\$VersionName\CursorUnreal"
    & $Uat BuildPlugin "-Plugin=$Plugin" "-Package=$Out" -TargetPlatforms=Win64 -Rocket
}

if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}
Write-Host "Build succeeded ($Mode, $VersionName)."
