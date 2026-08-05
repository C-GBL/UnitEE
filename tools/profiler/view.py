#!/usr/bin/env python3
"""Offline viewer for the on-target profiler's CSV (plan section 9, M13 task 1).

The runtime dumps one row per frame over host:; this turns that into the two
answers anyone actually wants from it:

    where did the frame go            per-zone mean/max, sorted by cost
    did we hit the target             frame-time distribution against 33.3 ms

and, because M13 task 3 is a list of optimisation passes that each have to be
justified, a compare mode that diffs two runs zone by zone. An optimisation
that cannot be shown to have moved a number is not an optimisation.

    python tools/profiler/view.py profile.csv
    python tools/profiler/view.py profile.csv --top 8
    python tools/profiler/view.py before.csv --compare after.csv

No third-party dependencies: this has to run in CI and on a machine that has
only just installed the toolchain.
"""

import argparse
import csv
import sys

# One NTSC frame is two fields at 59.94 Hz, so a vsync-locked frame measures
# 33.37 ms and is ON budget. What counts as a failure is a MISSED flip, which
# costs a whole field; the threshold sits between one period and two so a
# fraction of a percent of clock error cannot turn a healthy run red.
VSYNC_PERIOD_MS = 33.37
FRAME_BUDGET_MS = 40.0

# Columns that are counters rather than zone timings.
COUNTER_COLUMNS = {
    "frame", "frame_ms", "dma_build_ms", "dma_kick_ms", "dma_wait_ms",
    "pipeline_busy_ms", "dma_batches", "dma_qwords", "dma_overflows",
    "alloc_count", "alloc_bytes", "alloc_failures", "gc_pauses", "gc_pause_ms",
}


def load(path):
    """Returns (rows, zone_names). Rows are dicts of column to float."""
    try:
        with open(path, newline="") as handle:
            reader = csv.DictReader(handle)
            if reader.fieldnames is None:
                raise SystemExit("%s: empty file" % path)
            zones = [f for f in reader.fieldnames if f not in COUNTER_COLUMNS]
            rows = []
            for raw in reader:
                row = {}
                for key, value in raw.items():
                    if key is None or value is None:
                        continue
                    try:
                        row[key] = float(value)
                    except ValueError:
                        row[key] = 0.0
                rows.append(row)
    except OSError as exc:
        raise SystemExit("cannot read %s: %s" % (path, exc))
    if not rows:
        raise SystemExit("%s: no frames" % path)
    return rows, zones


def percentile(values, fraction):
    """Nearest-rank percentile. No numpy, and no interpolation games: with a
    hundred-odd frames the rank is the honest answer."""
    if not values:
        return 0.0
    ordered = sorted(values)
    index = int(round(fraction * (len(ordered) - 1)))
    return ordered[index]


def column(rows, name):
    return [r.get(name, 0.0) for r in rows]


def sparkline(values, width=60, top=None):
    """ASCII frame-time chart. Uses only '#', '-' and ' ' so it survives every
    terminal, log file and CI console this might be read in."""
    if not values:
        return ""
    ceiling = top if top else max(values) or 1.0
    # Downsample by taking the worst frame in each bucket: an average would
    # hide exactly the single-frame spike the chart exists to reveal.
    bucket = max(1, len(values) // width)
    buckets = [max(values[i:i + bucket]) for i in range(0, len(values), bucket)]
    rows = 8
    lines = []
    for level in range(rows, 0, -1):
        threshold = ceiling * level / rows
        line = "".join("#" if v >= threshold else " " for v in buckets)
        marker = ""
        if threshold >= FRAME_BUDGET_MS > ceiling * (level - 1) / rows:
            marker = "  <- %.1f ms budget" % FRAME_BUDGET_MS
            line = line.replace(" ", "-")
        lines.append("  %6.1f |%s%s" % (threshold, line, marker))
    lines.append("         +" + "-" * len(buckets))
    return "\n".join(lines)


def summarise(path, rows, zones, top_n):
    frames = column(rows, "frame_ms")
    over = [f for f in frames if f > FRAME_BUDGET_MS]
    mean = sum(frames) / len(frames)

    print("=" * 72)
    print("%s: %d frames" % (path, len(rows)))
    print("=" * 72)
    print("frame ms   mean %6.2f   median %6.2f   p95 %6.2f   worst %6.2f"
          % (mean, percentile(frames, 0.5), percentile(frames, 0.95), max(frames)))
    print("fps        mean %6.2f" % (1000.0 / mean if mean else 0.0))
    verdict = "PASS" if not over else "FAIL"
    print("dropped %d of %d frames (%.1f%%)   [%s: a flip is missed past "
          "%.1f ms, vsync period %.2f ms]"
          % (len(over), len(frames), 100.0 * len(over) / len(frames), verdict,
             FRAME_BUDGET_MS, VSYNC_PERIOD_MS))
    print()
    print(sparkline(frames, top=max(max(frames), FRAME_BUDGET_MS * 1.2)))
    print()

    # Where the frame went. Sorted by mean, because that is the one that pays
    # back optimisation; max is shown alongside because a spike is a
    # different bug from a cost.
    stats = []
    for zone in zones:
        values = column(rows, zone)
        if not any(values):
            continue
        stats.append((sum(values) / len(values), max(values), zone))
    stats.sort(reverse=True)

    if stats:
        # Inclusive time: a nested zone is counted in its parent too, so these
        # legitimately sum past 100%. Saying so beats a reader concluding the
        # numbers are broken.
        print("%-24s %8s %8s %8s   (inclusive: nested zones overlap)"
              % ("zone", "mean", "max", "% frame"))
        print("-" * 52)
        for mean_ms, max_ms, zone in stats[:top_n]:
            label = zone[:-3] if zone.endswith("_ms") else zone
            print("%-24s %8.3f %8.3f %7.1f%%"
                  % (label, mean_ms, max_ms, 100.0 * mean_ms / mean if mean else 0.0))
        print()

    wait = sum(column(rows, "dma_wait_ms")) / len(rows)
    build = sum(column(rows, "dma_build_ms")) / len(rows)
    pipeline = sum(column(rows, "pipeline_busy_ms")) / len(rows)
    print("pipeline   chain build %.3f ms   EE blocked waiting %.3f ms   "
          "in flight %.3f ms" % (build, wait, pipeline))
    # Plan section 15.3: if the EE is not idle at the end of the frame, the
    # EE is the bottleneck. Say which side to look at rather than making the
    # reader remember the rule.
    if wait > mean * 0.25:
        print("           the EE spends %.0f%% of the frame blocked: the "
              "bottleneck is VU1/GS, not the EE" % (100.0 * wait / mean))
    else:
        print("           the EE is busy rather than blocked: profile the EE "
              "first")

    overflows = sum(column(rows, "dma_overflows"))
    failures = sum(column(rows, "alloc_failures"))
    gc_pauses = sum(column(rows, "gc_pauses"))
    gc_ms = sum(column(rows, "gc_pause_ms"))
    print("allocation failures %d   chain overflows %d" % (failures, overflows))
    print("gc         %d pauses, %.2f ms total, %.3f ms per frame"
          % (gc_pauses, gc_ms, gc_ms / len(rows)))
    if failures:
        print("WARNING: allocation failures are a D4 breach on their own")
    return len(over) == 0 and failures == 0


def compare(path_a, rows_a, zones_a, path_b, rows_b, zones_b, top_n):
    """Diffs two runs zone by zone. This is the evidence an optimisation pass
    is supposed to produce."""
    def mean_of(rows, name):
        values = column(rows, name)
        return sum(values) / len(values) if values else 0.0

    frame_a = mean_of(rows_a, "frame_ms")
    frame_b = mean_of(rows_b, "frame_ms")

    print("=" * 72)
    print("compare")
    print("  before: %s (%d frames)" % (path_a, len(rows_a)))
    print("  after:  %s (%d frames)" % (path_b, len(rows_b)))
    print("=" * 72)
    delta = frame_b - frame_a
    print("frame ms   %.3f -> %.3f   %+.3f (%+.1f%%)"
          % (frame_a, frame_b, delta, 100.0 * delta / frame_a if frame_a else 0.0))
    print()

    shared = [z for z in zones_a if z in zones_b]
    rank = []
    for zone in shared:
        before = mean_of(rows_a, zone)
        after = mean_of(rows_b, zone)
        if before or after:
            rank.append((abs(after - before), before, after, zone))
    rank.sort(reverse=True)

    if rank:
        print("%-24s %9s %9s %9s" % ("zone", "before", "after", "delta"))
        print("-" * 55)
        for _, before, after, zone in rank[:top_n]:
            label = zone[:-3] if zone.endswith("_ms") else zone
            print("%-24s %9.3f %9.3f %+9.3f" % (label, before, after, after - before))
        print()

    if delta < -0.05:
        print("VERDICT: faster by %.3f ms per frame" % -delta)
    elif delta > 0.05:
        print("VERDICT: SLOWER by %.3f ms per frame" % delta)
    else:
        print("VERDICT: no measurable change (%.3f ms)" % delta)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv", help="profiler CSV dumped by the runtime")
    parser.add_argument("--compare", metavar="OTHER.CSV",
                        help="diff a second run against the first")
    parser.add_argument("--top", type=int, default=12,
                        help="how many zones to list (default 12)")
    parser.add_argument("--fail-over-budget", action="store_true",
                        help="exit non-zero if any frame missed the budget "
                             "or any allocation failed (for CI)")
    args = parser.parse_args()

    rows, zones = load(args.csv)
    if args.compare:
        other_rows, other_zones = load(args.compare)
        summarise(args.csv, rows, zones, args.top)
        print()
        summarise(args.compare, other_rows, other_zones, args.top)
        print()
        compare(args.csv, rows, zones, args.compare, other_rows, other_zones,
                args.top)
        return 0

    ok = summarise(args.csv, rows, zones, args.top)
    if args.fail_over_budget and not ok:
        print()
        print("FAIL: budget not met")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
