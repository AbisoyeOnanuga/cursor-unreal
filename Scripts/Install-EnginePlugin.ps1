<#
.SYNOPSIS
    Copies the packaged CursorUnreal plugin into your Unreal Engine 5.6 Plugins folder
    so every project on that engine can enable it without copying per project.

.DESCRIPTION
    Expects Packaged/<EngineVersion>/CursorUnreal/ from Build-Plugin.ps1 -Mode Package.
    Installs to <EngineRoot>\Engine\Plugins\Marketplace\CursorUnreal (Epic's usual place
    for third-party editor plugins). Closes nothing; quit the editor before running so
    DLLs are not locked. Runs npm install in Bridge for the Cursor provider.

.EXAMPLE
    ./Scripts/Build-Plugin.ps1 -Mode Package
    ./Scripts/Install-EnginePlugin.ps1
#>
[CmdletBinding()]
param(
    [string]$EngineRoot = "",
    [string]$PackageDir = "",
    [switch]$SkipNpm
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Project = Join-Path $RepoRoot "CursorUnrealDev.uproject"

function Resolve-EngineRoot {
    param([string]$Requested)
    if ($Requested) {
        if (-not (Test-Path (Join-Path $Requested "Engine\Build\Build.version"))) {
            throw "No Unreal Engine found at '$Requested'."
        }
        return (Resolve-Path $Requested).Path
    }
    $Association = (Get-Content $Project -Raw | ConvertFrom-Json).EngineAssociation
    $Key = "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$Association"
    $Installed = (Get-ItemProperty -Path $Key -ErrorAction SilentlyContinue).InstalledDirectory
    if ($Installed -and (Test-Path (Join-Path $Installed "Engine\Build\Build.version"))) {
        return $Installed
    }
    throw "Could not find Unreal Engine $Association. Pass -EngineRoot."
}

$EngineRoot = Resolve-EngineRoot $EngineRoot
$Version = Get-Content (Join-Path $EngineRoot "Engine\Build\Build.version") -Raw | ConvertFrom-Json
$VersionName = "{0}.{1}.{2}" -f $Version.MajorVersion, $Version.MinorVersion, $Version.PatchVersion

if (-not $PackageDir) {
    $PackageDir = Join-Path $RepoRoot "Packaged\$VersionName\CursorUnreal"
}
if (-not (Test-Path (Join-Path $PackageDir "CursorUnreal.uplugin"))) {
    throw "Packaged plugin not found at '$PackageDir'. Run: .\Scripts\Build-Plugin.ps1 -Mode Package"
}

$Dest = Join-Path $EngineRoot "Engine\Plugins\Marketplace\CursorUnreal"
Write-Host "Engine:  $EngineRoot ($VersionName)"
Write-Host "Source:  $PackageDir"
Write-Host "Install: $Dest"

$EditorRunning = [bool](Get-Process UnrealEditor*, UnrealEditor-Cmd* -ErrorAction SilentlyContinue)
if ($EditorRunning -and (Test-Path $Dest)) {
    throw "Close Unreal Editor first so the existing engine plugin DLLs are not locked, then run this script again."
}

$Marketplace = Split-Path $Dest -Parent
if (-not (Test-Path $Marketplace)) {
    New-Item -ItemType Directory -Force -Path $Marketplace | Out-Null
}
if (Test-Path $Dest) {
    Write-Host "Removing previous install..."
    Remove-Item -LiteralPath $Dest -Recurse -Force
}

# Program Files usually needs elevation.
try {
    Copy-Item -LiteralPath $PackageDir -Destination $Dest -Recurse -Force
}
catch {
    Write-Host "Copy needs administrator rights. Relaunching elevated..."
    $ArgList = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", "`"$PSCommandPath`"",
        "-EngineRoot", "`"$EngineRoot`"",
        "-PackageDir", "`"$PackageDir`""
    )
    if ($SkipNpm) { $ArgList += "-SkipNpm" }
    $Proc = Start-Process -FilePath "powershell.exe" -ArgumentList $ArgList -Verb RunAs -Wait -PassThru
    if ($Proc.ExitCode -ne 0) { throw "Elevated install failed (exit $($Proc.ExitCode))." }
    exit 0
}

if (-not $SkipNpm) {
    $Bridge = Join-Path $Dest "Bridge"
    if (Test-Path (Join-Path $Bridge "package.json")) {
        Write-Host "npm install in Bridge (Cursor provider)..."
        Push-Location $Bridge
        try {
            npm install --omit=dev
            if ($LASTEXITCODE -ne 0) { throw "npm install failed in $Bridge" }
        }
        finally {
            Pop-Location
        }
    }
}

Write-Host ""
Write-Host "Installed. Next:"
Write-Host "  1. Open any UE $VersionName project."
Write-Host "  2. Edit -> Plugins -> search 'Cursor Unreal' -> Enable -> restart if asked."
Write-Host "  3. Setup tab: Provider = Cursor. API key is shared (Windows Credential Manager);"
Write-Host "     re-enter only if this machine never stored CursorApiKey before."
Write-Host "  4. Provider choice is per-project; set Cursor once in each project's Setup tab."
Write-Host "Done."
