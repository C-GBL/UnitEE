# tools/installer/build.ps1
#
# Compiles UnitEE-Deps.nsi into UnitEE-Deps-Setup.exe.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools/installer/build.ps1
#   powershell ... -File build.ps1 -MakeNsis C:\portable\nsis-3.11\makensis.exe
#
# NSIS is not vendored. Install it from https://nsis.sourceforge.io/ (or unzip
# the portable nsis-<ver>.zip anywhere and pass -MakeNsis). The script needs
# nothing beyond stock NSIS: no third-party plugins, and no large-strings build.

[CmdletBinding()]
param(
    [string]$MakeNsis = "",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

function Fail([string]$Message) {
    Write-Host "ERROR: $Message"
    exit 1
}

$Here   = $PSScriptRoot
$Script = Join-Path $Here "UnitEE-Deps.nsi"
$Output = Join-Path $Here "UnitEE-Deps-Setup.exe"

if (-not (Test-Path $Script)) { Fail "UnitEE-Deps.nsi not found next to this script." }

if ([string]::IsNullOrEmpty($MakeNsis)) {
    $Candidates = @(
        "C:\Program Files (x86)\NSIS\makensis.exe",
        "C:\Program Files\NSIS\makensis.exe"
    )
    $OnPath = Get-Command makensis.exe -ErrorAction SilentlyContinue
    if ($null -ne $OnPath) { $Candidates = @($OnPath.Source) + $Candidates }
    $MakeNsis = $Candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if ([string]::IsNullOrEmpty($MakeNsis) -or -not (Test-Path $MakeNsis)) {
    Fail ("makensis.exe not found. Install NSIS from https://nsis.sourceforge.io/Download " +
          "or pass -MakeNsis <path to makensis.exe>.")
}

Write-Host "makensis: $MakeNsis"
& $MakeNsis /VERSION
Write-Host ""

if ($Clean -and (Test-Path $Output)) {
    Remove-Item -LiteralPath $Output -Force
}

# /WX turns warnings into errors. An NSIS warning is almost always a real
# mistake -- an unreachable section, a macro arg that silently expanded to
# nothing -- and this script is small enough to keep clean.
& $MakeNsis /WX /V3 $Script
if ($LASTEXITCODE -ne 0) { Fail "makensis failed (exit $LASTEXITCODE)." }

if (-not (Test-Path $Output)) { Fail "makensis reported success but $Output was not produced." }

$Size = [Math]::Round((Get-Item $Output).Length / 1KB, 1)
$Hash = (Get-FileHash -Algorithm SHA256 -Path $Output).Hash.ToLower()
Write-Host ""
Write-Host "Built $Output ($Size KB)"
Write-Host "  sha256: $Hash"
Write-Host ""
Write-Host "The installer downloads its payloads at run time, which is why it is small."
Write-Host "Attach it to a GitHub release; publish the sha256 alongside it."
