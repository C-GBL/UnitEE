# tools/git-hooks/install.ps1
#
# Installs this repository's git hooks. Hooks live in .git/hooks, which is not
# versioned, so every fresh clone must run this once.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools/git-hooks/install.ps1

[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$HookDir  = Join-Path $RepoRoot ".git\hooks"

if (-not (Test-Path $HookDir)) {
    Write-Host "ERROR: $HookDir not found. Is this a git repository?"
    exit 1
}

$Hooks = @("pre-commit")
foreach ($Hook in $Hooks) {
    $Src = Join-Path $PSScriptRoot $Hook
    $Dst = Join-Path $HookDir $Hook
    if (-not (Test-Path $Src)) {
        Write-Host "ERROR: source hook '$Src' is missing."
        exit 1
    }
    # Write with LF endings and no BOM: Git for Windows runs hooks through its
    # bundled sh, which fails on CRLF ("bad interpreter") and on a leading BOM.
    $Text = [System.IO.File]::ReadAllText($Src) -replace "`r`n", "`n"
    $Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Dst, $Text, $Utf8NoBom)
    Write-Host "Installed $Hook -> $Dst"
}

Write-Host ""
Write-Host "Done. The pre-commit hook enforces the ps2port.txt section 8 rule:"
Write-Host "  il2cpp-port/ holds only patches and our own files; build/ is never committed."
exit 0
