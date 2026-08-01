# tools/ps2dev/env.ps1
#
# Dot-source this in each build session to configure the ps2dev toolchain
# environment (plan section 4.1):
#
#   . tools/ps2dev/env.ps1
#
# Sets PS2DEV, PS2SDK, GSKIT and prepends the toolchain bin directories to
# PATH for the current session only. Windows PowerShell 5.1 compatible.

if ([string]::IsNullOrEmpty($env:PS2DEV)) {
    $env:PS2DEV = "C:\Users\Ash\ps2dev"
}
$env:PS2SDK = Join-Path $env:PS2DEV "ps2sdk"
$env:GSKIT  = Join-Path $env:PS2DEV "gsKit"

# Bin dirs from plan section 4.1.
$Ps2BinDirs = @(
    (Join-Path $env:PS2DEV "bin"),
    (Join-Path $env:PS2DEV "ee\bin"),
    (Join-Path $env:PS2DEV "iop\bin"),
    (Join-Path $env:PS2DEV "dvp\bin"),
    (Join-Path $env:PS2SDK "bin")
)

foreach ($Dir in $Ps2BinDirs) {
    $PathParts = $env:PATH -split ';'
    if ($PathParts -notcontains $Dir) {
        $env:PATH = "$Dir;$env:PATH"
    }
}

Write-Host "PS2DEV = $env:PS2DEV"
Write-Host "PS2SDK = $env:PS2SDK"
Write-Host "GSKIT  = $env:GSKIT"
Write-Host "PATH   = toolchain bin dirs prepended for this session."
