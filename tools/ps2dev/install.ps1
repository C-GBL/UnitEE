# tools/ps2dev/install.ps1
#
# Installs the prebuilt native-Windows ps2dev toolchain (plan section 4.1,
# option 2: prebuilt release tarball). Windows PowerShell 5.1 compatible.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools/ps2dev/install.ps1
#   powershell ... -File install.ps1 -Dest C:\ps2dev -TarballPath C:\dl\ps2dev-windows-latest.tar.gz
#
# After install, dot-source tools/ps2dev/env.ps1 in each build session.

[CmdletBinding()]
param(
    [string]$Dest = "C:\Users\Ash\ps2dev",
    [string]$TarballPath = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

function Fail([string]$Message) {
    Write-Host "ERROR: $Message"
    exit 1
}

$AssetName  = "ps2dev-windows-latest.tar.gz"
$ReleaseUrl = "https://github.com/ps2dev/ps2dev/releases/download/latest/$AssetName"
$ApiUrl     = "https://api.github.com/repos/ps2dev/ps2dev/releases/tags/latest"

# ---------------------------------------------------------------------------
# 1. Validate destination (plan section 4.1: absolute path, no spaces, no
#    non-Latin characters -- the ps2dev build scripts break otherwise).
# ---------------------------------------------------------------------------
if (-not [System.IO.Path]::IsPathRooted($Dest)) {
    Fail "Dest '$Dest' must be an absolute path."
}
if ($Dest -match '\s') {
    Fail "Dest '$Dest' contains whitespace. ps2dev requires a space-free install path (plan section 4.1)."
}
if ($Dest -match '[^\x21-\x7E]') {
    Fail "Dest '$Dest' contains non-ASCII characters. ps2dev requires a plain-Latin install path (plan section 4.1)."
}
if ((Test-Path $Dest) -and (@(Get-ChildItem -Path $Dest -Force).Count -gt 0)) {
    if (-not $Force) {
        Fail "Dest '$Dest' already exists and is not empty. Re-run with -Force to wipe and reinstall."
    }
    Write-Host "Removing existing install at $Dest ..."
    Remove-Item -Path $Dest -Recurse -Force -Confirm:$false
}

if ($null -eq (Get-Command curl.exe -ErrorAction SilentlyContinue)) { Fail "curl.exe not found on PATH." }
if ($null -eq (Get-Command tar.exe  -ErrorAction SilentlyContinue)) { Fail "tar.exe not found on PATH." }

$Staging = Join-Path $env:TEMP ("ps2dev-install-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $Staging -Force | Out-Null

# ---------------------------------------------------------------------------
# 2. Obtain the tarball
# ---------------------------------------------------------------------------
if ([string]::IsNullOrEmpty($TarballPath)) {
    $TarballPath = Join-Path $Staging $AssetName
    Write-Host "Downloading $ReleaseUrl ..."
    & curl.exe -L --fail --retry 3 --retry-delay 2 -o "$TarballPath" "$ReleaseUrl"
    if ($LASTEXITCODE -ne 0) { Fail "Download failed (curl exit $LASTEXITCODE)." }
} else {
    if (-not (Test-Path $TarballPath)) { Fail "TarballPath '$TarballPath' does not exist." }
    $TarballPath = (Resolve-Path $TarballPath).Path
    Write-Host "Using pre-downloaded tarball $TarballPath"
}

$LocalSha256 = (Get-FileHash -Algorithm SHA256 -Path $TarballPath).Hash.ToLower()
Write-Host "Local tarball sha256: $LocalSha256"

# ---------------------------------------------------------------------------
# 3. Extract into staging
# ---------------------------------------------------------------------------
$ExtractDir = Join-Path $Staging "extract"
New-Item -ItemType Directory -Path $ExtractDir -Force | Out-Null
Write-Host "Extracting ..."
# VERIFIED 2026-07-31: the Windows tarball carries a handful of POSIX symlinks
# under ps2sdk/ports/bin (bunzip, bzcat, bzcmp, bzegrep, bzfgrep, bzless) that
# Windows tar cannot create; it reports "Can't create ... Invalid argument" and
# exits 1 with everything else extracted correctly. Those six are bzip2 shell
# wrappers that no part of this project invokes, so a non-zero exit is only
# fatal if the toolchain itself is missing (checked by the layout probe below).
& tar.exe -xzf "$TarballPath" -C "$ExtractDir"
if ($LASTEXITCODE -ne 0) {
    Write-Host "NOTE: tar exited $LASTEXITCODE (expected: unsupported POSIX symlinks under ps2sdk/ports/bin)."
    Write-Host "      Continuing; the layout probe below is the real integrity check."
}

# ---------------------------------------------------------------------------
# 4. Normalize layout: tools must land at Dest directly (Dest\ee\bin\...),
#    whether or not the tarball wraps everything in a top-level directory.
# ---------------------------------------------------------------------------
$Root = $ExtractDir
if (-not (Test-Path (Join-Path $Root "ee\bin"))) {
    $TopLevel = @(Get-ChildItem -Path $ExtractDir -Force)
    if ($TopLevel.Count -eq 1 -and $TopLevel[0].PSIsContainer) {
        $Inner = $TopLevel[0].FullName
        if (Test-Path (Join-Path $Inner "ee\bin")) {
            Write-Host "Tarball has wrapping top-level dir '$($TopLevel[0].Name)'; unwrapping."
            $Root = $Inner
        }
    }
}
if (-not (Test-Path (Join-Path $Root "ee\bin"))) {
    Fail "Unexpected tarball layout: could not find ee\bin at the top level or one level down."
}

New-Item -ItemType Directory -Path $Dest -Force | Out-Null
Get-ChildItem -Path $Root -Force | ForEach-Object {
    Move-Item -Path $_.FullName -Destination $Dest -Force
}
Write-Host "Installed toolchain to $Dest"

# ---------------------------------------------------------------------------
# 5. Supply the MinGW runtime DLLs the toolchain needs to execute.
#
#    VERIFIED 2026-07-31 (the hard way): the ps2dev Windows tarball ships the
#    GCC 15.2.0 / binutils 2.45.1 binaries but NOT the MinGW runtime DLLs they
#    import. Every tool therefore dies instantly with exit -1073741515
#    (STATUS_DLL_NOT_FOUND) and no message. The imports were read out of the PE
#    headers of every .exe in the tree; the union is $RuntimeDlls below.
#
#    The binaries are 32-bit (PE machine 0x14c), so these must come from the
#    MSYS2 *mingw32* (i686) repo -- x86_64 DLLs load but fail at runtime.
#
#    These packages are zstd-compressed. Windows ships bsdtar as tar.exe, and
#    whether it reads .tar.zst depends entirely on the build: bsdtar 3.5.2 on
#    Windows 10 reports "zlib" as its only codec, then falls back to spawning an
#    external "zstd -d -qq" which is not present on a stock machine. So we
#    bootstrap a standalone zstd.exe rather than assume the local tar can cope.
#    Note the knot that unties: libzstd.dll below comes from a package that is
#    itself .tar.zst.
#
#    DLLs are copied into every directory containing an .exe: Windows resolves
#    imports from the executable's own directory first, which keeps the install
#    self-contained and independent of PATH ordering.
# ---------------------------------------------------------------------------
$RuntimeDlls = @(
    "libexpat-1.dll", "libgcc_s_dw2-1.dll", "libgmp-10.dll", "libiconv-2.dll",
    "libisl-23.dll", "liblzma-5.dll", "libmpc-3.dll", "libmpfr-6.dll",
    "libstdc++-6.dll", "libtermcap-0.dll", "libwinpthread-1.dll", "libzstd.dll"
)
# Pinned as of 2026-07-31. MSYS2 prunes superseded versions from the repo, so a
# 404 here is expected over time: PickPackage falls back to the newest matching
# package in the live directory index.
$MingwPkgs = @(
    "mingw-w64-i686-gcc-libs-16.1.0-5-any.pkg.tar.zst",
    "mingw-w64-i686-libwinpthread-git-12.0.0.r747.g1a99f8514-1-any.pkg.tar.zst",
    "mingw-w64-i686-libiconv-1.19-1-any.pkg.tar.zst",
    "mingw-w64-i686-expat-2.8.2-1-any.pkg.tar.zst",
    "mingw-w64-i686-gmp-6.3.0-2-any.pkg.tar.zst",
    "mingw-w64-i686-isl-0.28-1-any.pkg.tar.zst",
    "mingw-w64-i686-mpc-1.4.1-1-any.pkg.tar.zst",
    "mingw-w64-i686-mpfr-4.2.2-3-any.pkg.tar.zst",
    "mingw-w64-i686-xz-5.8.3-1-any.pkg.tar.zst",
    "mingw-w64-i686-termcap-1.3.1-7-any.pkg.tar.zst",
    "mingw-w64-i686-zstd-1.5.7-2-any.pkg.tar.zst"
)
$MingwRepo = "https://repo.msys2.org/mingw/mingw32/"
$DllStage  = Join-Path $Staging "mingw"
New-Item -ItemType Directory -Path $DllStage -Force | Out-Null

$RepoIndex = $null
function PickPackage([string]$PinnedName) {
    # Returns a package filename that actually exists in the repo today.
    $Probe = $null
    try {
        $Probe = Invoke-WebRequest -UseBasicParsing -Method Head -Uri ($MingwRepo + $PinnedName) `
                     -ErrorAction Stop
    } catch {
        $Probe = $null
    }
    if ($null -ne $Probe) { return $PinnedName }

    # Pinned version is gone: resolve the newest package with the same stem.
    $Stem = $PinnedName -replace '-[0-9][^-]*-[0-9]+-any\.pkg\.tar\.zst$', ''
    if ($Stem -eq $PinnedName) {
        $Stem = $PinnedName -replace '-[^-]+-[0-9]+-any\.pkg\.tar\.zst$', ''
    }
    if ($null -eq $script:RepoIndex) {
        Write-Host "  (fetching MSYS2 mingw32 package index)"
        $script:RepoIndex = (Invoke-WebRequest -UseBasicParsing -Uri $MingwRepo).Content
    }
    $Escaped = [regex]::Escape($Stem)
    $Matches = [regex]::Matches($script:RepoIndex, "$Escaped-[^`"<>]*?\.pkg\.tar\.zst") |
                   ForEach-Object { $_.Value } | Sort-Object -Unique
    if ($Matches.Count -eq 0) {
        Fail "MSYS2 package '$Stem' not found in $MingwRepo (pinned '$PinnedName' is gone)."
    }
    $Chosen = $Matches[$Matches.Count - 1]
    Write-Host "  pinned '$PinnedName' is gone; using '$Chosen'"
    return $Chosen
}

$ZstdUrl = "https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-v1.5.7-win64.zip"

function EnsureZstd {
    # bsdtar decompresses .tar.zst either with a linked-in libzstd or, failing
    # that, by spawning "zstd" from PATH. Returns nothing if the linked-in path
    # works; otherwise downloads a standalone zstd.exe and prepends it to PATH
    # so the fallback succeeds. Idempotent within a run.
    if ($null -ne (Get-Command zstd -ErrorAction SilentlyContinue)) { return }

    # bsdtar --version enumerates the codecs it was linked against, e.g.
    #   bsdtar 3.7.2 - libarchive 3.7.2 zlib/1.2.13 liblzma/5.4.4 libzstd/1.5.5
    # If zstd is in that list there is nothing to do. Anything that does not
    # advertise zstd -- the zlib-only bsdtar that ships with Windows 10, or GNU
    # tar, which always shells out -- needs a zstd.exe on PATH.
    $TarVersion = (& tar.exe --version 2>&1 | Out-String)
    if ($TarVersion -match 'zstd') { return }

    Write-Host "  tar.exe has no zstd codec; fetching a standalone zstd.exe ..."
    $ZstdDir = Join-Path $Staging "zstd"
    $ZstdZip = Join-Path $Staging "zstd-win64.zip"
    New-Item -ItemType Directory -Path $ZstdDir -Force | Out-Null
    & curl.exe -L --fail --retry 3 --retry-delay 2 -s -o "$ZstdZip" "$ZstdUrl"
    if ($LASTEXITCODE -ne 0) { Fail "Download of zstd.exe failed (curl exit $LASTEXITCODE): $ZstdUrl" }
    # A .zip, deliberately: Expand-Archive is built into PowerShell 5.1, so the
    # zstd bootstrap does not itself need a zstd.
    Expand-Archive -LiteralPath $ZstdZip -DestinationPath $ZstdDir -Force

    $ZstdExe = @(Get-ChildItem -Path $ZstdDir -Recurse -Filter "zstd.exe" -File)
    if ($ZstdExe.Count -eq 0) { Fail "zstd.exe was not found inside $ZstdUrl." }
    $env:PATH = $ZstdExe[0].DirectoryName + ";" + $env:PATH
    Write-Host "  using $($ZstdExe[0].FullName)"
}

Write-Host "Fetching MinGW runtime DLLs (the tarball does not ship them) ..."
EnsureZstd
foreach ($Pkg in $MingwPkgs) {
    $Actual  = PickPackage $Pkg
    $PkgPath = Join-Path $DllStage $Actual
    & curl.exe -L --fail --retry 3 --retry-delay 2 -s -o "$PkgPath" ($MingwRepo + $Actual)
    if ($LASTEXITCODE -ne 0) { Fail "Download of $Actual failed (curl exit $LASTEXITCODE)." }
    & tar.exe -xf "$PkgPath" -C "$DllStage"
    if ($LASTEXITCODE -ne 0) { Fail "Extraction of $Actual failed (tar exit $LASTEXITCODE)." }
}

$DllSource = @{}
foreach ($Dll in $RuntimeDlls) {
    $Found = @(Get-ChildItem -Path $DllStage -Recurse -Filter $Dll -File -ErrorAction SilentlyContinue)
    if ($Found.Count -eq 0) { Fail "Required runtime DLL '$Dll' was not present in any MSYS2 package." }
    $DllSource[$Dll] = $Found[0].FullName
}

$ExeDirs = @(Get-ChildItem -Path $Dest -Recurse -Filter *.exe -File |
                 ForEach-Object { $_.DirectoryName } | Sort-Object -Unique)
foreach ($Dir in $ExeDirs) {
    foreach ($Dll in $RuntimeDlls) {
        Copy-Item -Path $DllSource[$Dll] -Destination (Join-Path $Dir $Dll) -Force
    }
}
Write-Host "Placed $($RuntimeDlls.Count) runtime DLLs into $($ExeDirs.Count) tool directories."

# Prove the compiler actually runs before declaring success.
$EeGcc = Join-Path $Dest "ee\bin\mips64r5900el-ps2-elf-gcc.exe"
if (Test-Path $EeGcc) {
    $GccVersion = & $EeGcc --version 2>&1 | Select-Object -First 1
    if ($LASTEXITCODE -ne 0) {
        Fail "EE compiler is installed but will not execute (exit $LASTEXITCODE). Runtime DLLs may be wrong."
    }
    Write-Host "EE compiler OK: $GccVersion"
} else {
    Write-Host "WARNING: $EeGcc not found; skipping execution check."
}

# ---------------------------------------------------------------------------
# 6. Pin: record release asset updated_at + digest in PINNED.md.
#    Policy (see PINNED.md): the 'latest' tag is a moving target; every
#    install must leave an auditable record of exactly what was fetched.
# ---------------------------------------------------------------------------
$UpdatedAt = "unknown"
$Digest    = "unknown"
try {
    [Net.ServicePointManager]::SecurityProtocol = `
        [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $Release = Invoke-RestMethod -Uri $ApiUrl -Headers @{ "User-Agent" = "unity-ps2-install-script" }
    $Asset = $Release.assets | Where-Object { $_.name -eq $AssetName } | Select-Object -First 1
    if ($null -ne $Asset) {
        $UpdatedAt = "$($Asset.updated_at)"
        if ($Asset.PSObject.Properties.Name -contains "digest") {
            if ($null -ne $Asset.digest) { $Digest = "$($Asset.digest)" }
        }
    } else {
        Write-Host "WARNING: asset '$AssetName' not found in GitHub API response."
    }
} catch {
    Write-Host "WARNING: GitHub API query failed ($($_.Exception.Message)); pinning with local hash only."
}

$PinFile = Join-Path $PSScriptRoot "PINNED.md"
$Stamp = Get-Date -Format "yyyy-MM-ddTHH:mm:ss"
$PinLine = "| $Stamp | $AssetName | $UpdatedAt | $Digest | sha256:$LocalSha256 | $Dest |"
Add-Content -Path $PinFile -Value $PinLine -Encoding ASCII
Write-Host "Pinned install in $PinFile"
Write-Host "  $PinLine"

# ---------------------------------------------------------------------------
# 7. Rewrite CI-baked prefixes in pkg-config .pc files.
#    The ps2dev README documents this step as a sed incantation: the prebuilt
#    tarball's *.pc files contain absolute paths from the CI build machine
#    and must be rewritten to the actual install prefix.
# ---------------------------------------------------------------------------
$PcFiles = @(Get-ChildItem -Path $Dest -Recurse -Filter *.pc -File -ErrorAction SilentlyContinue)
if ($PcFiles.Count -eq 0) {
    Write-Host "NOTE: no .pc files found under $Dest (nothing to rewrite)."
} else {
    Write-Host "pkg-config files shipped with CI-baked prefixes (rewriting to $Dest):"
    $DestForward = $Dest.Replace('\', '/').TrimEnd('/')
    $Replacement = $DestForward.Replace('$', '$$')
    # Any absolute path (POSIX or drive-letter) whose last component is 'ps2dev'.
    $Pattern = '(?m)(?:[A-Za-z]:)?(?:/[^/\s:;,''"]+)*/ps2dev(?=[/\s]|$)'
    foreach ($Pc in $PcFiles) {
        Write-Host "  $($Pc.FullName)"
        $Text = [System.IO.File]::ReadAllText($Pc.FullName)
        $NewText = [regex]::Replace($Text, $Pattern, $Replacement)
        if ($NewText -ne $Text) {
            [System.IO.File]::WriteAllText($Pc.FullName, $NewText)
            Write-Host "    rewrote CI prefix -> $DestForward"
        } else {
            Write-Host "    WARNING: no absolute '*/ps2dev' prefix matched; inspect this file manually."
        }
    }
}

# ---------------------------------------------------------------------------
# 8. Cleanup + next steps
# ---------------------------------------------------------------------------
try {
    Remove-Item -Path $Staging -Recurse -Force -Confirm:$false -ErrorAction Stop
} catch {
    Write-Host "WARNING: could not remove staging dir $Staging"
}

Write-Host ""
Write-Host "Done. Next steps:"
Write-Host "  1. Dot-source the environment in each build session:"
Write-Host "       . `"$PSScriptRoot\env.ps1`""
Write-Host "  2. Verify: mips64r5900el-ps2-elf-gcc --version"
exit 0
