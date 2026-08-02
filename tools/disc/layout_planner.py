#!/usr/bin/env python3
"""Disc-layout planner (plan section 9, M10 task 4).

Orders the files on the ISO by the order a real run first touches them, so
the drive reads forward instead of seeking back and forth. mkps2iso lays
files out in the order the <file> elements appear in its XML script, which is
the LBA control the plan refers to.

Input is a first-access trace. The runtime records one (stream::trace_entry)
and sample 19 prints it, so either form works:

  * a raw dump, one path per line (stream::trace_dump), or
  * a PCSX2 console log, from which "M10_TRACE   <n> <path>" lines are
    scraped -- meaning a profiling run needs no extra plumbing to feed this.

Usage:
  python tools/disc/layout_planner.py --trace run.log --stage build/iso \\
      --elf SLPS_999.99 --serial SLPS-99999 --output build/disc.xml

  python tools/disc/layout_planner.py --trace run.log --stage build/iso \\
      --report-only

The report prints the seek cost of the planned order against the order the
staging directory would otherwise be burned in, in sectors travelled. That
number is the point of the whole exercise, so the tool states it rather than
asserting that things got better.
"""

import argparse
import os
import re
import sys
from xml.sax.saxutils import quoteattr

SECTOR_BYTES = 2048

# "[    1.6175] M10_TRACE   3 host:m5-scene.p2b" -- the index is discarded;
# the line order is the access order, and the recorder already dropped
# repeats. Deliberately NOT anchored at the start of the line: PCSX2 prefixes
# every console line with a timestamp, and this tool exists to read that log.
_LOG_LINE = re.compile(r"M10_TRACE\s+\d+\s+(\S.*?)\s*$")


def parse_trace(text):
    """Returns first-access file names, device prefixes stripped, in order."""
    lines = text.splitlines()
    logged = [m.group(1) for m in (_LOG_LINE.search(ln) for ln in lines) if m]
    if logged:
        raw = logged
    else:
        # A raw dump: one path per line. Lines that cannot be a disc path are
        # dropped rather than guessed at -- otherwise handing this tool an
        # emulator log with no M10_TRACE lines in it silently produces a
        # layout built out of log prose.
        raw = [ln.strip() for ln in lines
               if ln.strip() and not ln.lstrip().startswith("#")
               and looks_like_a_disc_path(ln.strip())]

    names = []
    for path in raw:
        name = basename_on_media(path)
        if name and name not in names:
            names.append(name)
    return names


def looks_like_a_disc_path(line):
    """Could this line be a file on a PS2 disc?

    ISO 9660 names have an extension and no spaces, so a line with a space in
    it or no dot in its last component is log prose, not a path.
    """
    if any(c.isspace() for c in line):
        return False
    name = basename_on_media(line)
    return "." in name and not name.startswith(".")


def basename_on_media(path):
    """'cdrom0:\\LEVEL2.P2B;1' -> 'LEVEL2.P2B'; 'host:a/b.p2b' -> 'b.p2b'."""
    # Strip a device prefix ("host:", "cdrom0:", "mass:") but not a drive
    # letter -- traces come from the console, where there are none.
    colon = path.find(":")
    if colon > 0:
        path = path[colon + 1:]
    path = path.replace("\\", "/")
    name = path.rsplit("/", 1)[-1]
    # ISO 9660 version suffix.
    semi = name.rfind(";")
    if semi > 0:
        name = name[:semi]
    return name.strip()


def stage_files(stage_dir):
    """Files in the staging directory, with sizes. Sorted for determinism."""
    out = []
    for entry in sorted(os.listdir(stage_dir)):
        full = os.path.join(stage_dir, entry)
        if os.path.isfile(full):
            out.append((entry, os.path.getsize(full)))
    return out


def plan_order(traced, staged, boot_elf=None):
    """First-access order first, then everything else, alphabetically.

    Returns (ordered_names, missing_from_stage). A traced file that is not
    staged is reported rather than dropped silently: it means the trace and
    the build disagree, which is worth knowing before burning a disc.
    """
    by_lower = {name.lower(): name for name, _ in staged}
    ordered = []
    missing = []

    # The boot ELF is read before anything the runtime can trace, so it goes
    # first whether or not it shows up in the trace.
    if boot_elf:
        actual = by_lower.get(boot_elf.lower())
        if actual is None:
            missing.append(boot_elf)
        else:
            ordered.append(actual)

    for name in traced:
        actual = by_lower.get(name.lower())
        if actual is None:
            missing.append(name)
        elif actual not in ordered:
            ordered.append(actual)

    for name, _ in staged:
        if name not in ordered:
            ordered.append(name)
    return ordered, missing


def seek_cost(order, sizes, traced):
    """Sectors travelled reading the traced files in the given layout.

    A file's start LBA is the sum of the (sector-rounded) sizes before it.
    The head starts at 0, so the first read counts too. This is a relative
    measure -- it ignores rotational latency and the real drive's caching --
    which is all it needs to be to compare two orders of the same files.
    """
    start = {}
    lba = 0
    for name in order:
        start[name] = lba
        lba += (sizes.get(name, 0) + SECTOR_BYTES - 1) // SECTOR_BYTES

    head = 0
    total = 0
    for name in traced:
        if name not in start:
            continue
        total += abs(start[name] - head)
        head = start[name] + (sizes.get(name, 0) + SECTOR_BYTES - 1) // SECTOR_BYTES
    return total


def write_xml(path, order, stage_dir, image_name, serial, volume, dummy_sectors):
    long_names = [n for n in order if len(n) > 31]
    lines = []
    lines.append("<!-- Generated by tools/disc/layout_planner.py.")
    lines.append("     File order IS the LBA order: files appear in the order a")
    lines.append("     profiled run first touched them, so the drive reads")
    lines.append("     forward. Regenerate from a new trace after changing")
    lines.append("     what the game loads; do not hand-sort this. -->")
    lines.append('<iso_project image_name=%s serial=%s region="america">'
                 % (quoteattr(image_name), quoteattr(serial)))
    lines.append("    <identifiers")
    lines.append('        system          ="PLAYSTATION"')
    lines.append('        application     ="PLAYSTATION"')
    lines.append("        volume          =%s" % quoteattr(volume))
    lines.append('        data_preparer   ="unity-ps2 layout_planner"')
    lines.append("    />")
    lines.append("    <layer>")
    lines.append("        <directory_tree source=%s>" % quoteattr(stage_dir))
    for name in order:
        lines.append("            <file name=%s/>" % quoteattr(name))
    if dummy_sectors > 0:
        # Pads the image out so the drive never runs off the end of the data.
        lines.append('            <dummy sectors="%d"/>' % dummy_sectors)
    lines.append("        </directory_tree>")
    lines.append("    </layer>")
    lines.append("</iso_project>")
    text = "\n".join(lines) + "\n"

    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write(text)
    return long_names


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--trace", required=True,
                    help="PCSX2 log with M10_TRACE lines, or a raw path dump")
    ap.add_argument("--stage", required=True,
                    help="directory holding the files to burn")
    ap.add_argument("--elf", default=None,
                    help="boot ELF file name; forced to the front of the disc")
    ap.add_argument("--output", default=None,
                    help="mkps2iso XML to write (omit for a report only)")
    ap.add_argument("--report-only", action="store_true",
                    help="print the plan and the seek numbers, write nothing")
    ap.add_argument("--image-name", default="game.iso")
    ap.add_argument("--serial", default="SLPS-99999")
    ap.add_argument("--volume", default="UNITYPS2")
    ap.add_argument("--dummy-sectors", type=int, default=0)
    args = ap.parse_args(argv)

    if not os.path.isdir(args.stage):
        sys.stderr.write("layout_planner: no such staging directory: %s\n"
                         % args.stage)
        return 2
    try:
        with open(args.trace, "r", encoding="utf-8", errors="replace") as f:
            traced = parse_trace(f.read())
    except OSError as e:
        sys.stderr.write("layout_planner: cannot read trace: %s\n" % e)
        return 2

    staged = stage_files(args.stage)
    if not staged:
        sys.stderr.write("layout_planner: staging directory is empty\n")
        return 2
    sizes = dict(staged)

    if not traced:
        sys.stderr.write(
            "layout_planner: the trace has no file accesses in it. Did the "
            "profiling run print M10_TRACE lines?\n")
        return 2

    ordered, missing = plan_order(traced, staged, args.elf)
    baseline = [name for name, _ in staged]
    planned_cost = seek_cost(ordered, sizes, traced)
    baseline_cost = seek_cost(baseline, sizes, traced)

    print("layout_planner: %d files staged, %d in the trace" %
          (len(staged), len(traced)))
    for i, name in enumerate(ordered):
        mark = "*" if name in traced or name == args.elf else " "
        print("  %3d %s %-32s %8d bytes" % (i, mark, name, sizes.get(name, 0)))
    if missing:
        print("layout_planner: WARNING -- traced but not staged, so the disc "
              "and the profiling run disagree:")
        for name in missing:
            print("    %s" % name)
    print("layout_planner: seek cost over the traced reads: %d sectors "
          "planned vs %d unplanned" % (planned_cost, baseline_cost))
    if baseline_cost > 0:
        print("layout_planner: %.1f%% of the seeking removed" %
              (100.0 * (baseline_cost - planned_cost) / baseline_cost))

    if args.report_only or args.output is None:
        return 0

    out_dir = os.path.dirname(os.path.abspath(args.output))
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    long_names = write_xml(args.output, ordered, args.stage, args.image_name,
                           args.serial, args.volume, args.dummy_sectors)
    print("layout_planner: wrote %s" % args.output)
    if long_names:
        # mkps2iso will reject these, so say so here rather than letting the
        # burn fail later with less context.
        print("layout_planner: WARNING -- names over 31 characters, which "
              "ISO 9660 cannot store:")
        for name in long_names:
            print("    %s" % name)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
