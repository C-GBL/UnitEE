# Disc layout planner

Orders files on the ISO by the order a real run first touches them, so the
drive reads forward instead of seeking (plan section 9, M10 task 4;
ADR-008). mkps2iso lays files out in the order its `<file>` elements appear,
which is the LBA control the plan refers to.

## Workflow

1. **Profile.** Run the game (or a sample) on PCSX2 or hardware and keep the
   console log. The runtime records first-access order for every file it
   opens, and samples print it as `M10_TRACE` lines:

   ```
   ./tools/ci/run-emu-test.sh build/ps2-release/samples/20-scene-stream/20-scene-stream.elf PS2UR_TOKEN_SCENESTREAM_OK
   ```

   The log lands in `$EMU_TEST_STAGE/emulog.txt` (default
   `/tmp/ps2-emu-stage/emulog.txt`). PCSX2's timestamp prefix is fine; the
   planner reads through it.

2. **Plan.** Point the planner at the log and the directory of files to
   burn:

   ```
   python tools/disc/layout_planner.py \
       --trace /tmp/ps2-emu-stage/emulog.txt \
       --stage build/iso \
       --elf SLUS_999.99 \
       --output build/disc.xml
   ```

   It prints the planned order, warns about anything traced but not staged
   (the trace and the build disagreeing), and reports the seek cost of the
   planned order against the unplanned one in sectors travelled. Add
   `--report-only` to see all that without writing a script.

3. **Burn.**

   ```
   tools/mkps2iso/mkps2iso.exe build/disc.xml
   ```

The boot ELF is forced to the front with `--elf`: it is read before the
runtime exists to trace anything.

## Testing

```
python tools/disc/test_layout_planner.py
```

Host-only, no emulator needed.

## Limits

`stream::kMaxTraceEntries` (64) caps how many distinct files a single run can
record. Raise it in `runtime/include/ps2ur/stream.h` before profiling a game
with more, or the tail of the layout falls back to alphabetical order.
