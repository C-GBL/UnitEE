#!/bin/sh
#
# tools/goldens/check.sh -- golden-image regression check (plan section 14.3).
#
# Boots an ELF that emits GOLDEN_TILE lines, then diffs its per-tile CRC32s
# against a checked-in golden. Tile granularity is the point: a failure says
# "tiles (3,5) and (4,5) changed" rather than "the image is different", which
# is the difference between a five-minute fix and an afternoon.
#
# Usage:
#   tools/goldens/check.sh <elf> <golden-file> [timeout]
#   tools/goldens/check.sh --update <elf> <golden-file> [timeout]
#
# --update rewrites the golden from the current run. Only do that when you
# have decided the new output is correct; that is a deliberate act, not a
# convenience.
#
# Exit: 0 match / 1 mismatch / 2 setup or capture problem.

UPDATE=0
if [ "$1" = "--update" ]; then
    UPDATE=1
    shift
fi

ELF=$1
GOLDEN=$2
TIMEOUT=${3:-90}

[ -n "$ELF" ] && [ -n "$GOLDEN" ] || {
    echo "usage: $0 [--update] <elf> <golden-file> [timeout]" >&2
    exit 2
}
[ -f "$ELF" ] || { echo "check: ELF not found: $ELF" >&2; exit 2; }

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
RUNNER="$ROOT/tools/ci/run-emu-test.sh"
# Work from the repository root. The golden's "# Files:" header names scenes
# relative to it, and those paths must stay free of spaces because the
# runner word-splits EMU_EXTRA_FILES; the checkout itself may not be.
ELF=$(cd "$(dirname "$ELF")" && pwd)/$(basename "$ELF")
case $GOLDEN in
    /*) ;;
    *) GOLDEN=$(pwd)/$GOLDEN ;;
esac
cd "$ROOT" || exit 2
[ -x "$RUNNER" ] || [ -f "$RUNNER" ] || {
    echo "check: $RUNNER missing" >&2; exit 2;
}

WORK=${GOLDEN_WORKDIR:-build/goldens}
mkdir -p "$WORK"
ACTUAL="$WORK/$(basename "$GOLDEN").actual"

# run-emu-test.sh handles the space-in-path staging and the absolute-path
# requirement; reuse it rather than duplicating that knowledge here.
echo "check: running $(basename "$ELF") ..."
# Wait for the sample's COMPLETION token (every golden sample prints
# PS2UR_TOKEN_*_OK or _FAIL after its last tile), not for the first
# GOLDEN_TILE line: the runner kills the emulator when its token appears,
# so waiting on GOLDEN_TILE raced the kill against the remaining tiles and
# captured a PREFIX of the image -- 64 or 116 of 02-scene-graph's 512,
# depending on flush timing. That was the "intermittent golden flake"
# (verify-log M12.5): not timing noise in the render, a truncated capture.
# A golden that needs a scene says so in its header:
#     # Files: build/m5-scene.p2b
# and the checker stages exactly that. It used to rely on whatever earlier
# runs had left in the emulator's staging directory, which worked until the
# directory was cleaned and every scene-reading golden failed at once with
# a message about CRCs (verify-log M13). An explicit EMU_EXTRA_FILES in the
# environment still wins, for --update runs on a golden that does not exist.
if [ -z "$EMU_EXTRA_FILES" ] && [ -f "$GOLDEN" ]; then
    GOLDEN_FILES=$(grep -m1 '^# Files:' "$GOLDEN" | sed 's/^# Files:[ ]*//')
    if [ -n "$GOLDEN_FILES" ]; then
        EMU_EXTRA_FILES=$GOLDEN_FILES
        export EMU_EXTRA_FILES
        echo "check: staging $GOLDEN_FILES"
    fi
fi
sh "$RUNNER" "$ELF" "PS2UR_TOKEN_" "$TIMEOUT" >/dev/null 2>&1
RC=$?

LOG=${EMU_TEST_STAGE:-/tmp/ps2-emu-stage}/emulog.txt
[ -f "$LOG" ] || LOG="$ROOT/build/emu-test/emulog.txt"
[ -f "$LOG" ] || { echo "check: no emulator log found" >&2; exit 2; }

# Header line plus the tile rows, stripped of the emulator's timestamps.
{
    grep -oE "GOLDEN [0-9]+x[0-9]+ at \([0-9]+,[0-9]+\) tiles [0-9]+x[0-9]+" "$LOG" | head -1
    grep -oE "GOLDEN_TILE [0-9]+ [0-9]+ [0-9A-F]{8}" "$LOG" | sed 's/^GOLDEN_TILE //'
} > "$ACTUAL"

TILES=$(grep -cE "^[0-9]+ [0-9]+ [0-9A-F]{8}$" "$ACTUAL" 2>/dev/null)
TILES=${TILES:-0}
if [ "$TILES" -eq 0 ]; then
    echo "check: FAILED to capture any tiles (emulator rc=$RC)." >&2
    MISSING=$(grep -ao "io: cannot open '[^']*'" "$LOG" | head -1)
    if [ -n "$MISSING" ]; then
        echo "       The ELF could not open its scene ($MISSING)." >&2
        echo "       Declare it in the golden: '# Files: build/<scene>.p2b'." >&2
    else
        echo "       The ELF must print GOLDEN_TILE <x> <y> <crc32> lines." >&2
    fi
    exit 2
fi
echo "check: captured $TILES tiles"

if [ "$UPDATE" -eq 1 ]; then
    FILES_LINE=""
    [ -f "$GOLDEN" ] && FILES_LINE=$(grep -m1 '^# Files:' "$GOLDEN")
    [ -z "$FILES_LINE" ] && [ -n "$EMU_EXTRA_FILES" ] && FILES_LINE="# Files: $EMU_EXTRA_FILES"
    {
        echo "# Golden tile CRC32s -- regenerated $(date -u +%Y-%m-%d) by tools/goldens/check.sh --update"
        echo "# Source ELF: $(basename "$ELF")"
        echo "# Format: <tile_x> <tile_y> <crc32>."
        [ -n "$FILES_LINE" ] && echo "$FILES_LINE"
        cat "$ACTUAL"
    } > "$GOLDEN"
    echo "check: golden UPDATED -> $GOLDEN"
    exit 0
fi

[ -f "$GOLDEN" ] || {
    echo "check: no golden at $GOLDEN. Create one with --update once the" >&2
    echo "       output has been confirmed correct." >&2
    exit 2
}

# Compare only the data rows; comments and capture dates must not matter.
EXPECTED_ROWS="$WORK/expected.rows"
ACTUAL_ROWS="$WORK/actual.rows"
grep -E "^[0-9]+ [0-9]+ [0-9A-F]{8}$" "$GOLDEN" | sort > "$EXPECTED_ROWS"
grep -E "^[0-9]+ [0-9]+ [0-9A-F]{8}$" "$ACTUAL" | sort > "$ACTUAL_ROWS"

if cmp -s "$EXPECTED_ROWS" "$ACTUAL_ROWS"; then
    echo "check: PASS -- all $TILES tiles match $GOLDEN"
    exit 0
fi

# One retry before failing, said out loud. 02-scene-graph mismatches
# intermittently under the software renderer -- an emulator capture-timing
# flake, not a rendering change (verify-log M12.5) -- and a REAL regression
# reproduces on the immediate re-run while the flake does not. A pass on
# retry is reported as exactly what it is.
if [ "${GOLDEN_RETRIED:-0}" -eq 0 ]; then
    echo "check: mismatch -- retrying once (known capture-timing flake," >&2
    echo "       verify-log M12.5). A real regression fails twice." >&2
    GOLDEN_RETRIED=1 exec sh "$0" "$ELF" "$GOLDEN" "$TIMEOUT"
fi

echo "check: FAIL -- tile CRCs differ from $GOLDEN (twice; this is real)"
echo "--- changed tiles (tile_x tile_y expected -> actual) ---"
join -j 1 \
    -o 0,1.2,2.2 \
    <(awk '{print $1"_"$2, $3}' "$EXPECTED_ROWS" | sort) \
    <(awk '{print $1"_"$2, $3}' "$ACTUAL_ROWS" | sort) 2>/dev/null |
    awk '$2 != $3 { split($1, t, "_"); printf "  tile(%s,%s)  %s -> %s\n", t[1], t[2], $2, $3 }'
echo ""
echo "If the change is intended, re-run with --update."
exit 1
