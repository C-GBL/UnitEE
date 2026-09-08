# UnitEE dependency installer

`UnitEE-Deps-Setup.exe` takes a fresh Windows machine to a green
**Window > PS2 > Toolchain** panel in one run. It installs everything the build
reaches for outside the Unity Editor, and it is the exact contract
`PS2ToolchainInfo.Discover` and `tools/ps2dev/doctor.sh` check against.

## What it installs

| Component | Version | From | Goes to | Sets |
|---|---|---|---|---|
| ps2dev toolchain | `latest` release, hash-pinned per install | github.com/ps2dev/ps2dev | `<root>\ps2dev` | `PS2DEV`, `PS2SDK`, `GSKIT`; PATH += 5 bin dirs |
| CMake | 4.4.3 | github.com/Kitware/CMake | `<root>\cmake` | PATH += `cmake\bin` |
| Ninja | 1.13.2 | github.com/ninja-build/ninja | `<root>\ninja` | PATH += `ninja` |
| Python | 3.13.15 | python.org (official installer, quiet) | its own default | its own PATH entry |
| .NET SDK | 9.0.304 | builds.dotnet.microsoft.com (quiet) | `C:\Program Files\dotnet` | its own PATH entry |
| mkps2iso | 1.1.1 | github.com/N4gtan/mkps2iso | `<root>\mkps2iso` | PATH += `mkps2iso` |
| PCSX2 | 2.8.2 | github.com/PCSX2/pcsx2 (Inno installer, silent) | `<root>\pcsx2` | `PCSX2` = path to `pcsx2-qt.exe` |

`<root>` defaults to `C:\UnitEE-Deps` and must contain no spaces (the ps2dev
build scripts fail on paths that do; the installer rejects such a path before
downloading anything).

The ps2dev component runs the project's own `tools/ps2dev/install.ps1`, so the
toolchain install is the same one a developer gets by hand, including the
`PINNED.md` audit row. The .NET SDK is pinned to **9.0.304** on purpose:
`il2cpp-port/build_m6.py` hardcodes that SDK's Roslyn path.

Components already found on PATH are unchecked automatically, so re-running is
cheap and never replaces a toolchain someone installed deliberately.

### What it cannot install

`il2cpp`, `libil2cpp` and the `unityaot-win32` class library live inside the
Unity Editor install. They come from the **IL2CPP scripting backend** component
in Unity Hub; no third-party installer can supply them. The finish page says so.

## Building it

Needs stock NSIS 3.x (https://nsis.sourceforge.io/Download); no plugins, no
large-strings build.

```
powershell -ExecutionPolicy Bypass -File tools/installer/build.ps1
powershell ... -File build.ps1 -MakeNsis C:\path\to\makensis.exe   # portable NSIS
```

Output is `tools/installer/UnitEE-Deps-Setup.exe`, about 100 KB, because every
payload is downloaded at install time. The build prints its sha256; publish
that next to the exe on the release. The exe is gitignored.

## Running it

Interactive: double-click, pick components, pick a root. Silent, for CI or a
provisioning script:

```
UnitEE-Deps-Setup.exe /S /COMPONENTS=ps2dev,cmake,ninja,python,dotnet,mkps2iso,pcsx2 /D=C:\UnitEE-Deps
```

`/COMPONENTS=` names the exact set (any subset of the seven above). Without it,
`/S` installs whatever the auto-detection left checked. `/D=` must be last and
unquoted -- NSIS's rule, not ours. Exit code 0 on success, 2 if a section
aborted.

Open a **new** terminal afterwards: PATH changes do not reach processes that
were already running.

## Uninstalling

**Apps & features > UnitEE Dependencies**, or `<root>\Uninstall.exe /S`. It
removes the PATH entries and the four variables it set, then the portable tools
under `<root>`. Python, the .NET SDK and PCSX2 ran their own installers and are
left alone -- deleting them silently could break something else on the machine.

## Updating a pin

Every fixed-version asset is pinned by URL and sha256 in `UnitEE-Deps.nsi`.
Bump the version, URL and hash together:

```powershell
(Get-FileHash -Algorithm SHA256 .\cmake-4.4.4-windows-x86_64.zip).Hash.ToLower()
```

A hash mismatch aborts that component on purpose. Never "fix" one by clearing
the pin. If a zip's top-level layout changes (wrapping directory or not), adjust
`-Unwrap` on that component's `fetch.ps1` call.

## How it is put together

- **`UnitEE-Deps.nsi`** -- UI, component selection, sequencing, the pin table.
  Nothing else; NSIS is a poor place for real logic.
- **`payload/fetch.ps1`** -- download (curl.exe, in Windows since 1803; the only
  bundled NSIS download plugin cannot do HTTPS), verify sha256, unzip, unwrap.
- **`payload/setenv.ps1`** -- PATH and variable edits, install and uninstall.
- **`build.ps1`** -- finds makensis, compiles with warnings as errors.

Things in there that look odd and are not:

- **PATH is edited by PowerShell, never by NSIS.** Stock NSIS caps strings at
  1024 characters (`NSIS_MAX_STRLEN`); the machine this was written on has a
  1755-character PATH. Doing the edit in the .nsi would truncate it.
- **PATH is written as `REG_EXPAND_SZ` via the registry**, not with
  `[Environment]::SetEnvironmentVariable`, which writes `REG_SZ` and permanently
  flattens every `%SystemRoot%`-style entry already there. The value is read
  with `DoNotExpandEnvironmentNames` for the same reason.
- **Lists are `|`-delimited strings**, not PowerShell arrays. `powershell -File`
  passes arguments literally, so `"a","b"` arrives as one string. `|` cannot
  occur in a path or a variable name, and nsExec uses `CreateProcess` directly
  so no shell ever sees it. Inside the script the split results go into
  differently named variables: assigning an array back to a `[string]`
  parameter makes PowerShell coerce it to a space-joined string.
- **The helpers pin their working directory to `%SystemRoot%`.** nsExec runs
  them with the CWD set to `$PLUGINSDIR`, where NSIS extracts its own native
  `System.dll` plugin. `Add-Type`'s C# compiler probes the CWD for reference
  assemblies and loaded that 32-bit plugin as if it were .NET's `System.dll`
  ("incorrect format"). The broadcast is also wrapped so a failure there can
  never abort an install whose variables are already written.
- **`SetRegView 64`** in both init callbacks. The installer is a 32-bit
  process; without this every `HKLM\Software` write is redirected to
  `WOW6432Node`, where Apps & features never finds the uninstall entry.
- **Every `MessageBox` carries `/SD IDOK`** so a silent install can never
  block on an invisible dialog.

## How it was verified

CMake, Ninja and mkps2iso were installed and uninstalled silently end to end
on Windows 10 with the machine-scope PATH: after the round trip, PATH was
byte-identical to a snapshot taken beforehand and the uninstall entry was gone
from the 64-bit registry view. The ps2dev component calls `install.ps1`, which
was verified separately against a scratch destination. The Python, .NET SDK
and PCSX2 sections were not run end to end -- they install real software
machine-wide -- but use each product's documented silent switches, and their
downloads and hashes were checked.
