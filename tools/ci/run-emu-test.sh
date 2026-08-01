#!/bin/sh
#
# tools/ci/run-emu-test.sh -- boot an ELF in PCSX2 headless and assert on the
# EE console output (plan section 14.2; used by M0 task 6 onward).
#
# Usage:
#   ./tools/ci/run-emu-test.sh <elf> [token] [timeout-seconds]
#
#   <elf>      PS2 executable to boot
#   [token]    string that must appear in the log (default: PASS)
#   [timeout]  seconds to let it run before killing (default: 30)
#
# Exit codes: 0 token found · 1 token absent · 2 setup problem (no emulator,
# no BIOS, ELF missing).
#
# The emulated program normally parks in SleepThread() and never exits, so this
# script always kills the emulator after the timeout; that is expected, not a
# failure. The verdict comes from the log, not the exit status of PCSX2.

ELF=$1
TOKEN=${2:-PASS}
TIMEOUT=${3:-30}

[ -n "$ELF" ] || { echo "usage: $0 <elf> [token] [timeout]" >&2; exit 2; }
[ -f "$ELF" ] || { echo "run-emu-test: ELF not found: $ELF" >&2; exit 2; }

# PCSX2 must be given an ABSOLUTE, host-native path. Two traps, both of which
# produce failures that look like guest-program crashes:
#
#  1. A RELATIVE path is resolved against the emulator's own 'host:' root,
#     which it derives from the ELF's own directory -- so it looks for
#     <elfdir>/<relative path>, fails with "Failed to read ELF from ''", then
#     executes garbage at pc=0x0 and floods the log with TLB misses.
#  2. A path containing a SPACE is rejected outright ("Requested boot ELF ...
#     does not exist") even when the file is plainly there. This repository
#     lives under "Unity 2 PS2", so that is the normal case here, not an edge
#     case. Rather than guess at the emulator's quoting rules, stage the ELF
#     into a space-free directory and boot that copy. The same constraint
#     applies to the toolchain itself (plan section 4.1 requires a space-free
#     PS2DEV), so a space-free staging area is the consistent answer.
ELF_ABS=$(cd "$(dirname "$ELF")" && pwd)/$(basename "$ELF")

case $ELF_ABS in
    *\ *)
        STAGE=${EMU_TEST_STAGE:-/tmp/ps2-emu-stage}
        mkdir -p "$STAGE" || { echo "run-emu-test: cannot create $STAGE" >&2; exit 2; }
        cp -f "$ELF_ABS" "$STAGE/" || { echo "run-emu-test: staging copy failed" >&2; exit 2; }
        ELF_ABS="$STAGE/$(basename "$ELF_ABS")"
        echo "run-emu-test: source path contains a space; booting staged copy at $ELF_ABS"
        ;;
esac

if command -v cygpath >/dev/null 2>&1; then
    # Git Bash: PCSX2 is a Windows binary and cannot read /c/... paths.
    ELF_ABS=$(cygpath -w "$ELF_ABS")
fi

# Locate PCSX2.
PCSX2_BIN=""
for c in "$PCSX2" "/c/Users/Ash/pcsx2/pcsx2-qt.exe" "$(command -v pcsx2-qt 2>/dev/null)"; do
    [ -n "$c" ] && [ -f "$c" ] && { PCSX2_BIN=$c; break; }
done
[ -n "$PCSX2_BIN" ] || {
    echo "run-emu-test: PCSX2 not found. Set \$PCSX2 to pcsx2-qt[.exe]." >&2
    exit 2
}

OUTDIR=${EMU_TEST_OUTDIR:-build/emu-test}
mkdir -p "$OUTDIR"
LOG="$OUTDIR/emulog.txt"
rm -f "$LOG"

# PCSX2 only writes EE printf output to the log when EnableEEConsole is set in
# its ini (default false). A silent log with a running ELF is almost always
# this, not a dead program -- check it before trusting a failure.
INI=${PCSX2_INI:-/c/Users/Ash/Documents/PCSX2/inis/PCSX2.ini}
if [ -f "$INI" ] && ! grep -q '^EnableEEConsole = true' "$INI" 2>/dev/null; then
    echo "run-emu-test: WARNING -- EnableEEConsole is not true in $INI;"
    echo "              EE printf output will not reach the log."
fi

echo "run-emu-test: booting $(basename "$ELF") (timeout ${TIMEOUT}s, token '$TOKEN')"

# -earlyconsolelog captures output from the very first instruction; without it
# the opening lines of a fast-booting ELF can be lost.
# The log path needs the same treatment as the ELF path.
LOG_ABS=$(cd "$(dirname "$LOG")" && pwd)/$(basename "$LOG")
case $LOG_ABS in
    *\ *)
        LOG_STAGE=${EMU_TEST_STAGE:-/tmp/ps2-emu-stage}
        mkdir -p "$LOG_STAGE"
        LOG_ABS="$LOG_STAGE/emulog.txt"
        LOG=$LOG_ABS
        rm -f "$LOG_ABS"
        ;;
esac
if command -v cygpath >/dev/null 2>&1; then
    LOG_ABS=$(cygpath -w "$LOG_ABS")
fi

"$PCSX2_BIN" -batch -nogui -earlyconsolelog -fastboot \
             -logfile "$LOG_ABS" -elf "$ELF_ABS" &
EMU_PID=$!

# Poll rather than sleeping the full timeout: a passing test finishes early.
i=0
FOUND=1
while [ "$i" -lt "$TIMEOUT" ]; do
    if [ -f "$LOG" ] && grep -qF "$TOKEN" "$LOG" 2>/dev/null; then
        FOUND=0
        break
    fi
    i=$((i + 1))
    sleep 1
done

kill "$EMU_PID" 2>/dev/null
wait "$EMU_PID" 2>/dev/null

if [ "$FOUND" -eq 0 ]; then
    echo "run-emu-test: PASS -- found '$TOKEN' after ${i}s"
    grep -F "$TOKEN" "$LOG" | sed 's/^/  /'
    exit 0
fi

echo "run-emu-test: FAIL -- '$TOKEN' not found in $LOG within ${TIMEOUT}s"
echo "--- last 20 log lines ---"
tail -n 20 "$LOG" 2>/dev/null | sed 's/^/  /'
exit 1
