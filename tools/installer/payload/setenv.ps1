# tools/installer/payload/setenv.ps1
#
# Persist environment variables and PATH entries for the UnitEE dependencies,
# and undo them on uninstall.
#
# Two things here are not incidental:
#
#   1. PATH is read and written through the registry as REG_EXPAND_SZ, not via
#      [Environment]::SetEnvironmentVariable. That API writes REG_SZ, which
#      permanently flattens any %SystemRoot%-style entry already in the user's
#      PATH -- a silent, machine-wide corruption that outlives this installer.
#
#   2. Nothing here goes through NSIS string handling. The stock NSIS build caps
#      strings at 1024 characters (NSIS_MAX_STRLEN=1024) and a real machine PATH
#      routinely exceeds that, so doing the edit in the .nsi would truncate it.
#
# Lists are one string with '|' between entries, not PowerShell arrays:
# powershell.exe -File passes arguments literally, so "a","b" arrives as the
# single string a","b. '|' cannot appear in a Windows path or a variable name,
# and needs no shell to survive because nsExec uses CreateProcess directly.
#
# Usage:
#   setenv.ps1 -AddPath "C:\UnitEE-Deps\cmake\bin|C:\UnitEE-Deps\ninja"
#   setenv.ps1 -SetVar "PS2DEV=C:\UnitEE-Deps\ps2dev|PCSX2=C:\...\pcsx2-qt.exe"
#   setenv.ps1 -RemovePath "C:\UnitEE-Deps\ninja" -RemoveVar "PS2DEV|PCSX2"

[CmdletBinding()]
param(
    [string]$AddPath = "",
    [string]$RemovePath = "",
    [string]$SetVar = "",      # NAME=VALUE entries
    [string]$RemoveVar = "",
    [ValidateSet("Machine", "User")][string]$Scope = "Machine"
)

function SplitList([string]$List) {
    if ([string]::IsNullOrWhiteSpace($List)) { return @() }
    return @($List -split '\|' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
}
# New names on purpose: assigning an array back to a [string] parameter makes
# PowerShell coerce it to one space-joined string, undoing the split.
$AddPaths    = SplitList $AddPath
$RemovePaths = SplitList $RemovePath
$SetVars     = SplitList $SetVar
$RemoveVars  = SplitList $RemoveVar

$ErrorActionPreference = "Stop"

# The caller (nsExec) runs us with the working directory set to the installer's
# $PLUGINSDIR, and NSIS extracts its own native System.dll plugin there. csc,
# which Add-Type invokes below, probes the current directory for reference
# assemblies and would load that 32-bit plugin as if it were .NET's System.dll:
#   "could not be opened -- An attempt was made to load a program with an
#    incorrect format."
# Both of these are needed: Set-Location moves PowerShell's location, and
# [Environment]::CurrentDirectory is what the compiler actually probes.
Set-Location -LiteralPath $env:SystemRoot
[Environment]::CurrentDirectory = $env:SystemRoot

if ($Scope -eq "Machine") {
    $RegPath = "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment"
} else {
    $RegPath = "HKCU:\Environment"
}

function Get-RawValue([string]$Name) {
    # DoNotExpandEnvironmentNames keeps %SystemRoot% as written instead of
    # handing back an expanded copy we would then persist.
    $Key = Get-Item -LiteralPath $RegPath
    return $Key.GetValue($Name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
}

function Normalize([string]$Entry) {
    if ($null -eq $Entry) { return "" }
    return $Entry.Trim().TrimEnd('\').ToLowerInvariant()
}

$Changed = $false

# --- PATH -----------------------------------------------------------------
if ($AddPaths.Count -gt 0 -or $RemovePaths.Count -gt 0) {
    $Raw = Get-RawValue "Path"
    if ($null -eq $Raw) { $Raw = "" }
    $Entries = @($Raw -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $Before = ($Entries -join ';')

    foreach ($R in $RemovePaths) {
        if ([string]::IsNullOrWhiteSpace($R)) { continue }
        $N = Normalize $R
        $Entries = @($Entries | Where-Object { (Normalize $_) -ne $N })
    }

    foreach ($A in $AddPaths) {
        if ([string]::IsNullOrWhiteSpace($A)) { continue }
        $N = Normalize $A
        $Present = @($Entries | Where-Object { (Normalize $_) -eq $N }).Count -gt 0
        if (-not $Present) {
            # Appended, not prepended: a dependency installer has no business
            # shadowing a toolchain the developer put on PATH themselves.
            $Entries += $A.TrimEnd('\')
            Write-Host "  PATH += $A"
        } else {
            Write-Host "  PATH already has $A"
        }
    }

    $After = ($Entries -join ';')
    if ($After -ne $Before) {
        Set-ItemProperty -LiteralPath $RegPath -Name "Path" -Value $After -Type ExpandString
        $Changed = $true
    }
}

# --- Named variables ------------------------------------------------------
foreach ($Pair in $SetVars) {
    if ([string]::IsNullOrWhiteSpace($Pair)) { continue }
    $Split = $Pair.IndexOf('=')
    if ($Split -lt 1) {
        [Console]::Error.WriteLine("ERROR: -SetVar entry '$Pair' is not NAME=VALUE.")
        exit 1
    }
    $Name  = $Pair.Substring(0, $Split)
    $Value = $Pair.Substring($Split + 1)
    Set-ItemProperty -LiteralPath $RegPath -Name $Name -Value $Value -Type String
    Write-Host "  $Name = $Value"
    $Changed = $true
}

foreach ($Name in $RemoveVars) {
    if ([string]::IsNullOrWhiteSpace($Name)) { continue }
    if ($null -ne (Get-RawValue $Name)) {
        Remove-ItemProperty -LiteralPath $RegPath -Name $Name -Force
        Write-Host "  removed $Name"
        $Changed = $true
    }
}

# --- Tell everything already running ---------------------------------------
if ($Changed) {
    # Without this, only processes started after the next sign-in see the change.
    # Explorer picks it up and passes it to anything launched from it afterwards.
    # A failed broadcast costs nothing but a sign-out; the variables are
    # already written. It must never fail the install.
    try {
        if (-not ("UnitEEEnvBroadcast" -as [type])) {
            Add-Type -Namespace "" -Name "UnitEEEnvBroadcast" -MemberDefinition @'
[DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Auto)]
public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg, UIntPtr wParam,
    string lParam, uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);
'@
        }
        $HWND_BROADCAST   = [IntPtr]0xffff
        $WM_SETTINGCHANGE = 0x1A
        $SMTO_ABORTIFHUNG = 0x2
        $Result = [UIntPtr]::Zero
        [void][UnitEEEnvBroadcast]::SendMessageTimeout(
            $HWND_BROADCAST, $WM_SETTINGCHANGE, [UIntPtr]::Zero, "Environment",
            $SMTO_ABORTIFHUNG, 5000, [ref]$Result)
        Write-Host "  broadcast WM_SETTINGCHANGE"
    } catch {
        Write-Host "  note: could not broadcast the environment change ($($_.Exception.Message))."
        Write-Host "        The variables are set; sign out and back in to pick them up."
    }
}

exit 0
