<#
.SYNOPSIS
    Runs Scripts/mcp-probe.mjs against a running editor, supplying the MCP token from Windows
    Credential Manager.

.DESCRIPTION
    The token is the one the plugin created for its own loopback MCP server ("CursorUnreal/McpToken").
    It is handed to the probe through an environment variable for that one process and is never
    printed, written to a file, or put on a command line.

.EXAMPLE
    ./Scripts/Run-McpProbe.ps1
    ./Scripts/Run-McpProbe.ps1 -Calls 100 -Label unfocused
#>
[CmdletBinding()]
param(
    [int]$Port = 18732,
    [int]$Calls = 30,
    [string]$Label = "",
    [switch]$Json,
    # Also ask to run a dangerous tool, to confirm it is refused. Headless editors only: with the
    # editor UI open this shows an approval prompt and waits for someone to answer it.
    [switch]$CheckApproval
)

$ErrorActionPreference = "Stop"

if (-not ("CursorUnrealCred.Native" -as [type])) {
    Add-Type -Namespace CursorUnrealCred -Name Native -MemberDefinition @"
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct CREDENTIAL {
    public uint Flags; public uint Type; public string TargetName; public string Comment;
    public System.Runtime.InteropServices.ComTypes.FILETIME LastWritten;
    public uint CredentialBlobSize; public IntPtr CredentialBlob; public uint Persist;
    public uint AttributeCount; public IntPtr Attributes; public string TargetAlias; public string UserName;
}
[DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool CredReadW(string target, uint type, uint flags, out IntPtr credential);
[DllImport("advapi32.dll")]
public static extern void CredFree(IntPtr credential);
"@
}

function Read-McpToken {
    $Pointer = [IntPtr]::Zero
    # 1 = CRED_TYPE_GENERIC
    if (-not [CursorUnrealCred.Native]::CredReadW("CursorUnreal/McpToken", 1, 0, [ref]$Pointer)) { return $null }
    try {
        $Credential = [Runtime.InteropServices.Marshal]::PtrToStructure($Pointer, [type][CursorUnrealCred.Native+CREDENTIAL])
        if ($Credential.CredentialBlobSize -eq 0) { return $null }
        $Bytes = New-Object byte[] $Credential.CredentialBlobSize
        [Runtime.InteropServices.Marshal]::Copy($Credential.CredentialBlob, $Bytes, 0, $Bytes.Length)
        return [Text.Encoding]::UTF8.GetString($Bytes)
    }
    finally { [CursorUnrealCred.Native]::CredFree($Pointer) }
}

$Token = Read-McpToken
if (-not $Token) {
    throw "No MCP token in Credential Manager yet. Start the editor once with the plugin enabled; it creates the token when the MCP server first starts."
}

$Probe = Join-Path $PSScriptRoot "mcp-probe.mjs"
$ProbeArgs = @($Probe, "--url", "http://127.0.0.1:$Port/mcp", "--calls", "$Calls")
if ($Label) { $ProbeArgs += @("--label", $Label) }
if ($Json) { $ProbeArgs += "--json" }
if ($CheckApproval) { $ProbeArgs += "--check-approval" }

$Previous = $env:CURSOR_UNREAL_MCP_TOKEN
try {
    $env:CURSOR_UNREAL_MCP_TOKEN = $Token
    & node @ProbeArgs
    $ExitCode = $LASTEXITCODE
}
finally {
    $env:CURSOR_UNREAL_MCP_TOKEN = $Previous
    $Token = $null
}
exit $ExitCode
