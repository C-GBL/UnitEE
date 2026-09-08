# tools/installer/payload/fetch.ps1
#
# Download one dependency, verify it, and put it where the installer wants it.
# Called once per component by UnitEE-Deps.nsi via nsExec. Kept out of the .nsi
# because NSIS strings cap at 1024 characters (stock build: NSIS_MAX_STRLEN)
# and because this is testable on its own, which a Section is not.
#
# Modes:
#   -Extract zip   unpack the archive into -Dest
#   -Extract none  leave the downloaded file at -Dest (an installer .exe)
#
# Exit codes: 0 success, non-zero with a message on stderr. The installer
# aborts the section on anything non-zero.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Url,
    [Parameter(Mandatory = $true)][string]$Dest,
    [string]$Sha256 = "",
    [ValidateSet("zip", "none")][string]$Extract = "zip",
    # The archive wraps its payload in a single top-level directory that we do
    # not want in the final path (cmake-4.4.3-windows-x86_64/, mkps2iso-1.1.1-win64/).
    [switch]$Unwrap,
    [string]$Label = ""
)

$ErrorActionPreference = "Stop"

# See setenv.ps1: we are launched with the working directory inside the
# installer's $PLUGINSDIR, which holds NSIS's own native plugin DLLs. Nothing
# here should ever resolve a file relative to that.
Set-Location -LiteralPath $env:SystemRoot
[Environment]::CurrentDirectory = $env:SystemRoot

function Fail([string]$Message) {
    [Console]::Error.WriteLine("ERROR: $Message")
    exit 1
}

if ([string]::IsNullOrEmpty($Label)) { $Label = Split-Path $Url -Leaf }

$Staging = Join-Path $env:TEMP ("unitee-dep-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $Staging -Force | Out-Null

try {
    $FileName = Split-Path $Url -Leaf
    if ($FileName -match '\?') { $FileName = ($FileName -split '\?')[0] }
    $Download = Join-Path $Staging $FileName

    Write-Host "  downloading $Label ..."
    # curl.exe has shipped in Windows since 1803 and is what tools/ps2dev/install.ps1
    # already relies on, so the installer needs no download plugin (NSISdl, the only
    # bundled one, cannot do HTTPS).
    & curl.exe -L --fail --retry 3 --retry-delay 2 -s -o "$Download" "$Url"
    if ($LASTEXITCODE -ne 0) { Fail "download of $Url failed (curl exit $LASTEXITCODE)." }
    if (-not (Test-Path $Download)) { Fail "download of $Url produced no file." }

    if (-not [string]::IsNullOrEmpty($Sha256)) {
        $Actual = (Get-FileHash -Algorithm SHA256 -Path $Download).Hash.ToLower()
        $Expected = $Sha256.ToLower()
        if ($Actual -ne $Expected) {
            Fail ("sha256 mismatch for $Label.`n  expected $Expected`n  actual   $Actual`n" +
                  "  The pinned release asset changed, or the download was tampered with. " +
                  "Re-pin deliberately in UnitEE-Deps.nsi; do not skip this check.")
        }
        Write-Host "  sha256 OK"
    }

    if ($Extract -eq "none") {
        $DestDir = Split-Path $Dest -Parent
        if (-not (Test-Path $DestDir)) { New-Item -ItemType Directory -Path $DestDir -Force | Out-Null }
        Move-Item -LiteralPath $Download -Destination $Dest -Force
        Write-Host "  saved $Dest"
        exit 0
    }

    $Unpack = Join-Path $Staging "unpack"
    New-Item -ItemType Directory -Path $Unpack -Force | Out-Null
    # Expand-Archive, not tar.exe: bsdtar reads zip, but GNU tar (which is what
    # tar resolves to when a Git install is ahead of System32 on PATH) does not.
    Expand-Archive -LiteralPath $Download -DestinationPath $Unpack -Force

    $Root = $Unpack
    if ($Unwrap) {
        $Top = @(Get-ChildItem -Path $Unpack -Force)
        if ($Top.Count -eq 1 -and $Top[0].PSIsContainer) {
            $Root = $Top[0].FullName
        } else {
            Write-Host "  note: -Unwrap asked for but the archive has no single top-level directory; using it as-is."
        }
    }

    if (Test-Path $Dest) { Remove-Item -LiteralPath $Dest -Recurse -Force -Confirm:$false }
    New-Item -ItemType Directory -Path $Dest -Force | Out-Null
    Get-ChildItem -Path $Root -Force | ForEach-Object {
        Move-Item -LiteralPath $_.FullName -Destination $Dest -Force
    }
    Write-Host "  installed $Label to $Dest"
    exit 0
}
catch {
    Fail $_.Exception.Message
}
finally {
    Remove-Item -Path $Staging -Recurse -Force -ErrorAction SilentlyContinue
}
