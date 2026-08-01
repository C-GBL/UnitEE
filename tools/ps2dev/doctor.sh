#!/bin/sh
#
# tools/ps2dev/doctor.sh -- verify the toolchain (plan section 9, M0 task 3).
#
# "Every later build step calls this first." Prints a table and exits non-zero
# if anything REQUIRED is missing. Optional tools are reported but do not fail
# the run, so the script is usable well before M12 (ISO packaging) exists.
#
# Runs under Git Bash on Windows and under sh on Linux/macOS. On Windows the
# toolchain binaries carry a .exe suffix; probing handles both.
#
# Usage:  ./tools/ps2dev/doctor.sh [-q]
#   -q   quiet: print only failures and the final verdict.

QUIET=0
[ "$1" = "-q" ] && QUIET=1

# --- defaults matching tools/ps2dev/env.ps1 and tools/cmake/ps2-toolchain.cmake
: "${PS2DEV:=/c/Users/Ash/ps2dev}"
: "${PS2SDK:=$PS2DEV/ps2sdk}"
: "${GSKIT:=$PS2DEV/gsKit}"

# Accept a Windows-style PS2DEV (C:/Users/...) by translating it for sh tests.
win_to_posix() {
    printf '%s' "$1" | sed -E 's|^([A-Za-z]):|/\L\1|; s|\\|/|g'
}
PS2DEV_P=$(win_to_posix "$PS2DEV")
PS2SDK_P=$(win_to_posix "$PS2SDK")
GSKIT_P=$(win_to_posix "$GSKIT")

FAILURES=0
WARNINGS=0

say() { [ "$QUIET" -eq 1 ] || printf '%s\n' "$1"; }
row() { [ "$QUIET" -eq 1 ] || printf '  %-34s %-8s %s\n' "$1" "$2" "$3"; }

# resolve <name> -- echo the path to a tool, trying $2 as an explicit dir first,
# then PATH; tolerates a .exe suffix.
resolve() {
    _name=$1
    _dir=$2
    for _cand in "$_dir/$_name" "$_dir/$_name.exe"; do
        [ -n "$_dir" ] && [ -f "$_cand" ] && { printf '%s' "$_cand"; return 0; }
    done
    command -v "$_name" 2>/dev/null && return 0
    return 1
}

check_dir() {
    _label=$1; _path=$2; _required=$3
    if [ -d "$_path" ]; then
        row "$_label" "OK" "$_path"
    elif [ "$_required" = "required" ]; then
        row "$_label" "MISSING" "$_path"
        FAILURES=$((FAILURES + 1))
    else
        row "$_label" "absent" "$_path (optional)"
        WARNINGS=$((WARNINGS + 1))
    fi
}

# check_tool <label> <binary> <dir> <required> <version-args>
check_tool() {
    _label=$1; _bin=$2; _dir=$3; _required=$4; _vargs=$5
    _path=$(resolve "$_bin" "$_dir") || _path=""
    if [ -z "$_path" ]; then
        if [ "$_required" = "required" ]; then
            row "$_label" "MISSING" "$_bin not found (looked in $_dir and PATH)"
            FAILURES=$((FAILURES + 1))
        else
            row "$_label" "absent" "$_bin (optional)"
            WARNINGS=$((WARNINGS + 1))
        fi
        return
    fi
    if [ -n "$_vargs" ]; then
        # A tool that is present but cannot execute is the failure mode the
        # ps2dev Windows tarball ships with (missing MinGW runtime DLLs -- it
        # dies with STATUS_DLL_NOT_FOUND and no message). Detect it here.
        _ver=$("$_path" $_vargs 2>&1 | head -n 1)
        if [ -z "$_ver" ]; then
            row "$_label" "BROKEN" "$_path exists but produced no output"
            say "        -> on Windows this is usually missing MinGW runtime DLLs;"
            say "           re-run tools/ps2dev/install.ps1, which fetches them."
            FAILURES=$((FAILURES + 1))
            return
        fi
        row "$_label" "OK" "$_ver"
    else
        row "$_label" "OK" "$_path"
    fi
}

say ""
say "ps2dev doctor -- toolchain verification (plan section 9, M0)"
say ""
say "Environment"
row "PS2DEV" "-" "$PS2DEV"
row "PS2SDK" "-" "$PS2SDK"
row "GSKIT"  "-" "$GSKIT"
say ""

say "Directories"
check_dir "PS2DEV root"      "$PS2DEV_P"                 required
check_dir "PS2SDK"           "$PS2SDK_P"                 required
check_dir "ps2sdk ee/include" "$PS2SDK_P/ee/include"     required
check_dir "ps2sdk ee/lib"    "$PS2SDK_P/ee/lib"          required
check_dir "ps2sdk ee/startup" "$PS2SDK_P/ee/startup"     required
check_dir "gsKit"            "$GSKIT_P"                  optional
say ""

say "Compilers and tools"
check_tool "EE C compiler"   "mips64r5900el-ps2-elf-gcc" "$PS2DEV_P/ee/bin"  required "--version"
check_tool "EE C++ compiler" "mips64r5900el-ps2-elf-g++" "$PS2DEV_P/ee/bin"  required "--version"
check_tool "EE archiver"     "mips64r5900el-ps2-elf-ar"  "$PS2DEV_P/ee/bin"  required "--version"
check_tool "IOP compiler"    "mipsel-none-elf-gcc"       "$PS2DEV_P/iop/bin" required "--version"
check_tool "VU assembler"    "dvp-as"                    "$PS2DEV_P/dvp/bin" required "--version"
check_tool "ps2-packer"      "ps2-packer"                "$PS2DEV_P/bin"     optional ""
check_tool "ps2client"       "ps2client"                 "$PS2DEV_P/bin"     optional ""
# mkps2iso is NOT part of the ps2dev distribution (github.com/N4gtan/mkps2iso).
# Only needed from M12 (ISO packaging), so it is optional until then.
check_tool "mkps2iso"        "mkps2iso"                  ""                  optional ""
say ""

say "Host build tools"
check_tool "CMake"  "cmake"  "" required "--version"
check_tool "Ninja"  "ninja"  "" required "--version"
check_tool "Python" "python" "" required "--version"
check_tool "dotnet" "dotnet" "" required "--version"
say ""

say "Emulator"
# PCSX2 is not on PATH in a normal install; check the known location too.
_pcsx2=""
for _c in "$PCSX2" "/c/Users/Ash/pcsx2/pcsx2-qt.exe" "$(command -v pcsx2-qt 2>/dev/null)"; do
    [ -n "$_c" ] && [ -f "$_c" ] && { _pcsx2=$_c; break; }
done
if [ -n "$_pcsx2" ]; then
    row "PCSX2" "OK" "$_pcsx2"
else
    row "PCSX2" "absent" "set \$PCSX2 or install to /c/Users/Ash/pcsx2 (optional)"
    WARNINGS=$((WARNINGS + 1))
fi
# A BIOS is required to boot anything, but must never be committed or
# downloaded (plan section 9 M0 notes, section 17.3). Report only.
_bios_dir="${PCSX2_BIOS:-/c/Users/Ash/Documents/PCSX2/bios}"
if [ -d "$_bios_dir" ] && [ -n "$(ls -A "$_bios_dir" 2>/dev/null)" ]; then
    row "PS2 BIOS" "OK" "$_bios_dir (user-provided; never committed)"
else
    row "PS2 BIOS" "absent" "$_bios_dir -- provide your own dump; do NOT download one"
    WARNINGS=$((WARNINGS + 1))
fi
say ""

if [ "$FAILURES" -gt 0 ]; then
    printf 'doctor: FAILED -- %d required item(s) missing, %d optional absent.\n' \
        "$FAILURES" "$WARNINGS"
    printf 'Install the toolchain with: powershell -File tools/ps2dev/install.ps1\n'
    exit 1
fi

printf 'doctor: OK -- all required tools present'
[ "$WARNINGS" -gt 0 ] && printf ' (%d optional absent)' "$WARNINGS"
printf '.\n'
exit 0
