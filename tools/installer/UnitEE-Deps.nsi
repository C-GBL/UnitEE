; tools/installer/UnitEE-Deps.nsi
;
; Installer for every external dependency a UnitEE build reaches for, so a new
; machine goes from nothing to a green Window > PS2 > Toolchain panel in one run.
;
; What it covers, and why each one is here (PS2ToolchainInfo.Discover and
; tools/ps2dev/doctor.sh are the contract this installer satisfies):
;
;   ps2dev     EE/IOP/VU cross-toolchain. Located via the PS2DEV env var.
;   CMake      Required. The native build is CMake+Ninja; CMakeLists asks 3.24+.
;   Ninja      Required by doctor.sh; a note (not an error) in the Editor panel.
;   Python     Required. tools/bindgen and tools/disc/layout_planner.py.
;   .NET SDK   Required. il2cpp-port/build_m6.py shells out to dotnet's Roslyn.
;   mkps2iso   Optional. No ISO without it; the ELF still builds and host-boots.
;   PCSX2      Optional. Build And Run only.
;
; NOT covered, and it cannot be: il2cpp, libil2cpp and the unityaot BCL live
; inside the Unity Editor install. They come from the IL2CPP scripting backend
; component in Unity Hub. The finish page says so.
;
; Design notes worth keeping:
;   * Payloads are downloaded at install time, not bundled. Bundling would make
;     a ~600 MB artifact of other people's binaries, with the redistribution
;     questions that implies. Downloads go through curl.exe (in Windows since
;     1803) because NSISdl, the only bundled download plugin, cannot do HTTPS.
;   * Every asset is pinned by sha256 and verified in fetch.ps1.
;   * PATH and env vars are written by payload/setenv.ps1, never here: stock
;     NSIS caps strings at 1024 chars (NSIS_MAX_STRLEN) and real machine PATHs
;     exceed that, so doing it here would truncate someone's PATH.
;   * No third-party NSIS plugins. Stock makensis compiles this as-is.
;
; Build:  powershell -ExecutionPolicy Bypass -File tools/installer/build.ps1

Unicode true

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"

;----------------------------------------------------------------------------
; Product
;----------------------------------------------------------------------------
!define PRODUCT_NAME    "UnitEE Dependencies"
!define PRODUCT_VERSION "1.0.0"
!define PRODUCT_PUBLISHER "UnitEE"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\UnitEE-Deps"

Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "UnitEE-Deps-Setup.exe"
InstallDir "C:\UnitEE-Deps"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show
SetCompressor /SOLID lzma

VIProductVersion "1.0.0.0"
VIAddVersionKey "ProductName"     "${PRODUCT_NAME}"
VIAddVersionKey "FileDescription" "Installs the PS2 toolchain and host build tools for UnitEE"
VIAddVersionKey "FileVersion"     "${PRODUCT_VERSION}"
VIAddVersionKey "ProductVersion"  "${PRODUCT_VERSION}"
VIAddVersionKey "LegalCopyright"  "Downloads third-party tools from their official sources"

;----------------------------------------------------------------------------
; Pinned payloads -- the single source of truth for versions.
;
; To bump one: change the version, URL and sha256 together. Get the hash with
;   (Get-FileHash -Algorithm SHA256 <file>).Hash.ToLower()
; A mismatch aborts the section on purpose; never "fix" it by clearing the pin.
;----------------------------------------------------------------------------
!define CMAKE_VER "4.4.3"
!define CMAKE_URL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VER}/cmake-${CMAKE_VER}-windows-x86_64.zip"
!define CMAKE_SHA "4d52ebab7193a698651639ed80d8d04fd903358843572cf44c7fd234cb7c26ab"

!define NINJA_VER "1.13.2"
!define NINJA_URL "https://github.com/ninja-build/ninja/releases/download/v${NINJA_VER}/ninja-win.zip"
!define NINJA_SHA "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65"

!define MKPS2ISO_VER "1.1.1"
!define MKPS2ISO_URL "https://github.com/N4gtan/mkps2iso/releases/download/v${MKPS2ISO_VER}/mkps2iso-${MKPS2ISO_VER}-win64.zip"
!define MKPS2ISO_SHA "6c9501816c0e4da4860344bde114bc8b88230df475ad523af7ba3118e7e30181"

!define PYTHON_VER "3.13.15"
!define PYTHON_URL "https://www.python.org/ftp/python/${PYTHON_VER}/python-${PYTHON_VER}-amd64.exe"
!define PYTHON_SHA "edec09c4853aeae9ac36efb8c9f95b6b8e2fee65eee56d9767a8b7c69c574403"

; 9.0.304 exactly, not "latest 9.0": il2cpp-port/build_m6.py hardcodes
; C:/Program Files/dotnet/sdk/9.0.304/Roslyn/bincore/csc.dll.
!define DOTNET_VER "9.0.304"
!define DOTNET_URL "https://builds.dotnet.microsoft.com/dotnet/Sdk/${DOTNET_VER}/dotnet-sdk-${DOTNET_VER}-win-x64.exe"
!define DOTNET_SHA "23844407357522b98eb5fc774e4a444bbd209c4604d41b9094fc80eb89ab6a5e"

!define PCSX2_VER "2.8.2"
!define PCSX2_URL "https://github.com/PCSX2/pcsx2/releases/download/v${PCSX2_VER}/pcsx2-v${PCSX2_VER}-windows-x64-installer.exe"
!define PCSX2_SHA "3422f3705ccc641b7e4fdf3f280d635d05c3c8b53a56c3af55d9df115bdcf768"

;----------------------------------------------------------------------------
; State
;----------------------------------------------------------------------------
Var PSExe            ; 64-bit powershell.exe, resolved once in .onInit
Var Ps2DevDir
Var Pcsx2Exe
Var PathAdds         ; '|'-separated dirs to append to machine PATH (see setenv.ps1)
Var VarSets          ; '|'-separated NAME=VALUE pairs
Var Found
Var Components       ; /COMPONENTS= from the command line, silent installs

;----------------------------------------------------------------------------
; UI
;----------------------------------------------------------------------------
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Install the UnitEE build dependencies"
!define MUI_WELCOMEPAGE_TEXT "This installs the PlayStation 2 cross-toolchain and the host build tools a UnitEE build needs.$\r$\n$\r$\nEverything is downloaded from its official source during the install and checked against a pinned SHA-256, so an internet connection is required. Roughly 600 MB is downloaded if you select everything.$\r$\n$\r$\nTools you already have are unchecked automatically.$\r$\n$\r$\nThe Unity-side pieces (il2cpp, libil2cpp, the AOT class library) are part of the Unity Editor and cannot be installed from here -- add the IL2CPP scripting backend in Unity Hub."

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE ValidateInstallDir
!define MUI_DIRECTORYPAGE_TEXT_TOP "Choose where the portable tools (ps2dev, CMake, Ninja, mkps2iso, PCSX2) are installed.$\r$\n$\r$\nThe path must contain no spaces: the ps2dev build scripts fail on paths that do. Python and the .NET SDK ignore this and use their own standard locations."
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_TITLE "Dependencies installed"
!define MUI_FINISHPAGE_TEXT "Open a new terminal (or sign out and back in) so the updated PATH is picked up, then check the result:$\r$\n$\r$\n    tools\ps2dev\doctor.sh$\r$\n$\r$\nIn Unity, the same checks live under Window > PS2 > Toolchain.$\r$\n$\r$\nIf that panel still reports il2cpp, libil2cpp or the AOT BCL as missing, install the IL2CPP scripting backend for your Editor in Unity Hub -- those ship with Unity and no installer can supply them."
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

;----------------------------------------------------------------------------
; Helper macros
;----------------------------------------------------------------------------

; Run one of the bundled PowerShell helpers from $PLUGINSDIR, echoing its
; output into the details window. Leaves the exit code in $0.
!macro RunHelper SCRIPT ARGS
  nsExec::ExecToLog '"$PSExe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\${SCRIPT}" ${ARGS}'
  Pop $0
!macroend

!macro AbortIfFailed WHAT
  ${If} $0 != 0
    DetailPrint "FAILED: ${WHAT} (exit $0)"
    MessageBox MB_ICONSTOP|MB_OK "${WHAT} failed.$\r$\n$\r$\nSee the details window for the reason. You can re-run this installer and select only the parts that failed." /SD IDOK
    Abort "${WHAT} failed."
  ${EndIf}
!macroend

; Queue a PATH addition / env var for the -Post section, which applies them all
; in one pass so every registry write happens in one place.
!macro QueuePath DIR
  ${If} $PathAdds == ""
    StrCpy $PathAdds "${DIR}"
  ${Else}
    StrCpy $PathAdds "$PathAdds|${DIR}"
  ${EndIf}
!macroend

!macro QueueVar PAIR
  ${If} $VarSets == ""
    StrCpy $VarSets "${PAIR}"
  ${Else}
    StrCpy $VarSets "$VarSets|${PAIR}"
  ${EndIf}
!macroend

;----------------------------------------------------------------------------
; Sections
;----------------------------------------------------------------------------

Section "PS2 toolchain (ps2dev)" SEC_PS2DEV
  AddSize 1200000
  StrCpy $Ps2DevDir "$INSTDIR\ps2dev"
  DetailPrint "Installing the ps2dev cross-toolchain to $Ps2DevDir (downloads ~258 MB) ..."

  ; install.ps1 goes somewhere durable rather than $PLUGINSDIR: it appends its
  ; audit row to PINNED.md next to itself, and the developer needs env.ps1 to
  ; dot-source per build session.
  SetOutPath "$INSTDIR\ps2dev-tools"
  File "..\ps2dev\install.ps1"
  File "..\ps2dev\env.ps1"
  File "..\ps2dev\PINNED.md"
  File "..\ps2dev\doctor.sh"

  nsExec::ExecToLog '"$PSExe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\ps2dev-tools\install.ps1" -Dest "$Ps2DevDir" -Force'
  Pop $0
  !insertmacro AbortIfFailed "ps2dev toolchain install"

  ; PS2DEV must be set explicitly: PS2ToolchainLocator's fallback is a
  ; hardcoded C:/Users/Ash/ps2dev, which is correct on exactly one machine.
  !insertmacro QueueVar "PS2DEV=$Ps2DevDir"
  !insertmacro QueueVar "PS2SDK=$Ps2DevDir\ps2sdk"
  !insertmacro QueueVar "GSKIT=$Ps2DevDir\gsKit"
  !insertmacro QueuePath "$Ps2DevDir\bin"
  !insertmacro QueuePath "$Ps2DevDir\ee\bin"
  !insertmacro QueuePath "$Ps2DevDir\iop\bin"
  !insertmacro QueuePath "$Ps2DevDir\dvp\bin"
  !insertmacro QueuePath "$Ps2DevDir\ps2sdk\bin"
SectionEnd

Section "CMake ${CMAKE_VER}" SEC_CMAKE
  AddSize 120000
  DetailPrint "Installing CMake ${CMAKE_VER} ..."
  !insertmacro RunHelper "fetch.ps1" '-Url "${CMAKE_URL}" -Sha256 "${CMAKE_SHA}" -Dest "$INSTDIR\cmake" -Extract zip -Unwrap -Label "CMake ${CMAKE_VER}"'
  !insertmacro AbortIfFailed "CMake install"
  !insertmacro QueuePath "$INSTDIR\cmake\bin"
SectionEnd

Section "Ninja ${NINJA_VER}" SEC_NINJA
  AddSize 700
  DetailPrint "Installing Ninja ${NINJA_VER} ..."
  ; ninja-win.zip is a bare ninja.exe with no wrapping directory, so no -Unwrap.
  !insertmacro RunHelper "fetch.ps1" '-Url "${NINJA_URL}" -Sha256 "${NINJA_SHA}" -Dest "$INSTDIR\ninja" -Extract zip -Label "Ninja ${NINJA_VER}"'
  !insertmacro AbortIfFailed "Ninja install"
  !insertmacro QueuePath "$INSTDIR\ninja"
SectionEnd

Section "Python ${PYTHON_VER}" SEC_PYTHON
  AddSize 120000
  DetailPrint "Installing Python ${PYTHON_VER} (official installer, quiet) ..."
  !insertmacro RunHelper "fetch.ps1" '-Url "${PYTHON_URL}" -Sha256 "${PYTHON_SHA}" -Dest "$PLUGINSDIR\python-setup.exe" -Extract none -Label "Python ${PYTHON_VER}"'
  !insertmacro AbortIfFailed "Python download"
  DetailPrint "  running the Python installer ..."
  ExecWait '"$PLUGINSDIR\python-setup.exe" /quiet InstallAllUsers=1 PrependPath=1 Include_test=0 Include_launcher=1' $0
  ; 3010 is "installed, reboot required" and is not a failure.
  ${If} $0 != 0
  ${AndIf} $0 != 3010
    DetailPrint "FAILED: Python installer exit $0"
    MessageBox MB_ICONSTOP|MB_OK "The Python installer returned $0.$\r$\n$\r$\nIf Python is already installed from python.org, uncheck it and re-run." /SD IDOK
    Abort "Python install failed."
  ${EndIf}
  DetailPrint "  Python installed (it manages its own PATH entry)"
SectionEnd

Section ".NET SDK ${DOTNET_VER}" SEC_DOTNET
  AddSize 800000
  DetailPrint "Installing .NET SDK ${DOTNET_VER} (downloads ~227 MB) ..."
  !insertmacro RunHelper "fetch.ps1" '-Url "${DOTNET_URL}" -Sha256 "${DOTNET_SHA}" -Dest "$PLUGINSDIR\dotnet-sdk.exe" -Extract none -Label ".NET SDK ${DOTNET_VER}"'
  !insertmacro AbortIfFailed ".NET SDK download"
  DetailPrint "  running the .NET SDK installer ..."
  ExecWait '"$PLUGINSDIR\dotnet-sdk.exe" /install /quiet /norestart' $0
  ${If} $0 != 0
  ${AndIf} $0 != 3010
    DetailPrint "FAILED: .NET SDK installer exit $0"
    MessageBox MB_ICONSTOP|MB_OK "The .NET SDK installer returned $0." /SD IDOK
    Abort ".NET SDK install failed."
  ${EndIf}
  DetailPrint "  .NET SDK installed (it manages its own PATH entry)"
SectionEnd

Section "mkps2iso ${MKPS2ISO_VER}" SEC_MKPS2ISO
  AddSize 900
  DetailPrint "Installing mkps2iso ${MKPS2ISO_VER} ..."
  !insertmacro RunHelper "fetch.ps1" '-Url "${MKPS2ISO_URL}" -Sha256 "${MKPS2ISO_SHA}" -Dest "$INSTDIR\mkps2iso" -Extract zip -Unwrap -Label "mkps2iso ${MKPS2ISO_VER}"'
  !insertmacro AbortIfFailed "mkps2iso install"
  ; The build also accepts <repo>/tools/mkps2iso/mkps2iso.exe; PATH is the
  ; variant that works regardless of where the repo is checked out.
  !insertmacro QueuePath "$INSTDIR\mkps2iso"
SectionEnd

Section "PCSX2 ${PCSX2_VER} (emulator)" SEC_PCSX2
  AddSize 200000
  DetailPrint "Installing PCSX2 ${PCSX2_VER} ..."
  !insertmacro RunHelper "fetch.ps1" '-Url "${PCSX2_URL}" -Sha256 "${PCSX2_SHA}" -Dest "$PLUGINSDIR\pcsx2-setup.exe" -Extract none -Label "PCSX2 ${PCSX2_VER}"'
  !insertmacro AbortIfFailed "PCSX2 download"
  DetailPrint "  running the PCSX2 installer ..."
  ; PCSX2 ships an Inno Setup installer; these are Inno's silent switches.
  ExecWait '"$PLUGINSDIR\pcsx2-setup.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR="$INSTDIR\pcsx2"' $0
  ${If} $0 != 0
    DetailPrint "FAILED: PCSX2 installer exit $0"
    MessageBox MB_ICONEXCLAMATION|MB_OK "The PCSX2 installer returned $0. Build still works; only Build And Run needs PCSX2." /SD IDOK
  ${Else}
    StrCpy $Pcsx2Exe "$INSTDIR\pcsx2\pcsx2-qt.exe"
    ${If} ${FileExists} "$Pcsx2Exe"
      ; Set PCSX2 explicitly rather than relying on the locator's guesses.
      !insertmacro QueueVar "PCSX2=$Pcsx2Exe"
      DetailPrint "  PCSX2 at $Pcsx2Exe"
    ${Else}
      DetailPrint "  note: pcsx2-qt.exe not found under $INSTDIR\pcsx2; set the PCSX2 variable by hand."
    ${EndIf}
  ${EndIf}
SectionEnd

Section "-Post"
  ; Away from $PLUGINSDIR before running a helper: that directory holds NSIS's
  ; own plugin DLLs, one of which is named System.dll and is not a .NET
  ; assembly. Anything that resolves a reference relative to the working
  ; directory picks it up by mistake. setenv.ps1 defends itself too.
  SetOutPath "$INSTDIR"

  ; One pass over the registry for everything the selected sections queued.
  ${If} $PathAdds != ""
  ${OrIf} $VarSets != ""
    DetailPrint "Updating the machine environment ..."
    StrCpy $1 ""
    ${If} $PathAdds != ""
      StrCpy $1 '$1 -AddPath "$PathAdds"'
    ${EndIf}
    ${If} $VarSets != ""
      StrCpy $1 '$1 -SetVar "$VarSets"'
    ${EndIf}
    !insertmacro RunHelper "setenv.ps1" "-Scope Machine $1"
    !insertmacro AbortIfFailed "environment update"
  ${EndIf}

  ; Keep setenv.ps1 so the uninstaller can undo the PATH edits.
  File "payload\setenv.ps1"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName"     "${PRODUCT_NAME}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion"  "${PRODUCT_VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher"       "${PRODUCT_PUBLISHER}"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1

  DetailPrint ""
  DetailPrint "Open a NEW terminal before building: PATH changes do not reach already-running processes."
SectionEnd

;----------------------------------------------------------------------------
; Component descriptions
;----------------------------------------------------------------------------
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_PS2DEV}   "The EE, IOP and VU cross-compilers, ps2sdk and gsKit. Runs the project's own tools/ps2dev/install.ps1 and sets PS2DEV, PS2SDK and GSKIT. Downloads ~258 MB. Required to build anything."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_CMAKE}    "CMake ${CMAKE_VER}, portable. The native build is CMake+Ninja and the project's CMakeLists require 3.24 or newer. Required."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_NINJA}    "Ninja ${NINJA_VER}. Without it CMake falls back to a slower generator that does not stream build output."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_PYTHON}   "Python ${PYTHON_VER} from python.org. Runs the bridge generator (tools/bindgen) and the disc layout planner. Required."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_DOTNET}   ".NET SDK ${DOTNET_VER}. il2cpp-port/build_m6.py drives the SDK's Roslyn compiler and expects this exact version. Required."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_MKPS2ISO} "mkps2iso ${MKPS2ISO_VER}, for ISO authoring with control over file order on the disc. Optional: without it the ELF still builds and boots over host: in PCSX2, but no game.iso is produced."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_PCSX2}    "PCSX2 ${PCSX2_VER}, and sets the PCSX2 variable to it. Optional: only Build And Run uses the emulator. A PS2 BIOS is not included and must be dumped from your own console."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

;----------------------------------------------------------------------------
; Functions (after the sections: .onInit references the section IDs)
;----------------------------------------------------------------------------

; In: tool name on the stack. Out: $Found = 1 if it resolves on PATH, else 0.
Function DetectOnPath
  Exch $1
  Push $2
  Push $3
  nsExec::ExecToStack '"$SYSDIR\where.exe" $1'
  Pop $2      ; exit code
  Pop $3      ; captured output, discarded
  ${If} $2 == "0"
    StrCpy $Found 1
  ${Else}
    StrCpy $Found 0
  ${EndIf}
  Pop $3
  Pop $2
  Pop $1
FunctionEnd

; Uncheck a section and relabel it when the tool is already on PATH, so a
; re-run is cheap and nothing the developer installed deliberately is stomped.
!macro SkipIfPresent EXE SEC LABEL
  Push "${EXE}"
  Call DetectOnPath
  ${If} $Found == 1
    SectionSetFlags ${SEC} 0
    SectionSetText  ${SEC} "${LABEL} (already on PATH)"
  ${EndIf}
!macroend

; In: needle, then haystack on the stack. Out: $Found = 1 if present.
; NSIS has no substring test in the stock headers, and pulling in StrFunc for
; one comparison is not worth the include.
Function StrContains
  Exch $1        ; haystack
  Exch
  Exch $2        ; needle
  Push $3
  Push $4
  Push $5
  StrLen $4 $2
  StrCpy $3 0
  StrCpy $Found 0
  ${Do}
    StrCpy $5 $1 $4 $3
    ${If} $5 == ""
      ${Break}
    ${EndIf}
    ${If} $5 == $2
      StrCpy $Found 1
      ${Break}
    ${EndIf}
    IntOp $3 $3 + 1
  ${Loop}
  Pop $5
  Pop $4
  Pop $3
  Pop $2
  Pop $1
FunctionEnd

; Select a section only when its name appears in /COMPONENTS=. Comparing with
; commas on both sides keeps "ninja" from matching a hypothetical "ninja-x".
!macro SelectIfNamed NAME SEC
  Push ",${NAME},"
  Push "$Components"
  Call StrContains
  ${If} $Found == 1
    SectionSetFlags ${SEC} ${SF_SELECTED}
  ${Else}
    SectionSetFlags ${SEC} 0
  ${EndIf}
!macroend

Function ValidateInstallDir
  ; ps2dev's own constraint, enforced here rather than 250 MB into the download.
  Push $0
  StrCpy $0 0
  loop:
    StrCpy $1 "$INSTDIR" 1 $0
    ${If} $1 == ""
      Goto done
    ${EndIf}
    ${If} $1 == " "
      MessageBox MB_ICONSTOP|MB_OK "The install path must not contain spaces.$\r$\n$\r$\n'$INSTDIR' does. The ps2dev build scripts fail on paths with spaces, so this is rejected here rather than midway through the toolchain install.$\r$\n$\r$\nTry something like C:\UnitEE-Deps." /SD IDOK
      Pop $0
      Abort
    ${EndIf}
    IntOp $0 $0 + 1
    Goto loop
  done:
  Pop $0
FunctionEnd

Function .onInit
  ; A 32-bit process writing HKLM\Software is redirected to WOW6432Node, where
  ; Apps & features would never find the uninstall entry.
  SetRegView 64

  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP|MB_OK "UnitEE requires 64-bit Windows: the .NET SDK, PCSX2 and the Unity Editor are all x64." /SD IDOK
    Abort
  ${EndIf}

  ; The installer is 32-bit, so $WINDIR\System32 would be redirected to
  ; SysWOW64. Sysnative is a 32-bit process's door to the real System32.
  StrCpy $PSExe "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
  ${IfNot} ${FileExists} "$PSExe"
    StrCpy $PSExe "$WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe"
  ${EndIf}
  ${IfNot} ${FileExists} "$PSExe"
    MessageBox MB_ICONSTOP|MB_OK "Windows PowerShell was not found. This installer drives its downloads through it." /SD IDOK
    Abort
  ${EndIf}

  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File "payload\fetch.ps1"
  File "payload\setenv.ps1"

  StrCpy $PathAdds ""
  StrCpy $VarSets ""

  !insertmacro SkipIfPresent "cmake"    ${SEC_CMAKE}    "CMake ${CMAKE_VER}"
  !insertmacro SkipIfPresent "ninja"    ${SEC_NINJA}    "Ninja ${NINJA_VER}"
  !insertmacro SkipIfPresent "python"   ${SEC_PYTHON}   "Python ${PYTHON_VER}"
  !insertmacro SkipIfPresent "dotnet"   ${SEC_DOTNET}   ".NET SDK ${DOTNET_VER}"
  !insertmacro SkipIfPresent "mkps2iso" ${SEC_MKPS2ISO} "mkps2iso ${MKPS2ISO_VER}"

  ; /COMPONENTS=cmake,ninja,mkps2iso picks the set explicitly, which is the
  ; only way to drive a silent install: without it, /S installs the defaults.
  ;   UnitEE-Deps-Setup.exe /S /COMPONENTS=ps2dev,cmake,ninja /D=C:\deps
  ; (/D must come last and cannot be quoted -- that is NSIS's own rule.)
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/COMPONENTS=" $R1
  ${IfNot} ${Errors}
    StrCpy $Components ",$R1,"
    !insertmacro SelectIfNamed "ps2dev"   ${SEC_PS2DEV}
    !insertmacro SelectIfNamed "cmake"    ${SEC_CMAKE}
    !insertmacro SelectIfNamed "ninja"    ${SEC_NINJA}
    !insertmacro SelectIfNamed "python"   ${SEC_PYTHON}
    !insertmacro SelectIfNamed "dotnet"   ${SEC_DOTNET}
    !insertmacro SelectIfNamed "mkps2iso" ${SEC_MKPS2ISO}
    !insertmacro SelectIfNamed "pcsx2"    ${SEC_PCSX2}
  ${EndIf}
FunctionEnd

;----------------------------------------------------------------------------
; Uninstall
;----------------------------------------------------------------------------
Function un.onInit
  SetRegView 64
  StrCpy $PSExe "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
  ${IfNot} ${FileExists} "$PSExe"
    StrCpy $PSExe "$WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe"
  ${EndIf}
FunctionEnd

Section "Uninstall"
  DetailPrint "Removing the environment entries ..."
  ${If} ${FileExists} "$INSTDIR\setenv.ps1"
    StrCpy $1 "$INSTDIR\cmake\bin|$INSTDIR\ninja|$INSTDIR\mkps2iso"
    StrCpy $2 "$INSTDIR\ps2dev\bin|$INSTDIR\ps2dev\ee\bin|$INSTDIR\ps2dev\iop\bin|$INSTDIR\ps2dev\dvp\bin|$INSTDIR\ps2dev\ps2sdk\bin"
    nsExec::ExecToLog '"$PSExe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\setenv.ps1" -Scope Machine -RemovePath "$1|$2" -RemoveVar "PS2DEV|PS2SDK|GSKIT|PCSX2"'
    Pop $0
  ${EndIf}

  ; Only what this installer unpacked. Python, the .NET SDK and PCSX2 ran their
  ; own installers and are removed through Apps & features -- silently deleting
  ; them here could break something else on the machine that depends on them.
  RMDir /r "$INSTDIR\cmake"
  RMDir /r "$INSTDIR\ninja"
  RMDir /r "$INSTDIR\mkps2iso"
  RMDir /r "$INSTDIR\ps2dev"
  RMDir /r "$INSTDIR\ps2dev-tools"
  Delete "$INSTDIR\setenv.ps1"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  DeleteRegKey HKLM "${UNINST_KEY}"

  MessageBox MB_ICONINFORMATION|MB_OK "The UnitEE dependencies were removed.$\r$\n$\r$\nPython, the .NET SDK and PCSX2 were installed by their own installers and are still present -- remove them from Apps & features if you no longer want them." /SD IDOK
SectionEnd
