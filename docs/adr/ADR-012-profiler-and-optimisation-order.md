# ADR-012: The profiler is always compiled in, and optimisation order comes from it

Status: accepted (M13 tasks 1, 2, 3). Plan 9 M13, 15.1, 15.3.

## Decision

Instrumentation is compiled into every build, including release. Zones cost
two reads of the EE cycle counter and a few adds; the overlay is off until a
button turns it on, and the CSV dump only happens in a profiling build. The
alternative, a separate instrumented configuration, measures a binary nobody
ships.

The optimisation list in plan section 9 M13 task 3 is a list of CANDIDATES
ordered by expected return, and expected is not measured. Each one is
implemented only after the profiler shows it has headroom in a real scene.

## Why always-on instrumentation

A profiler you have to opt into is a profiler that is out of date. This one
allocates nothing (zones live in a fixed table, per-frame allocation counts
are a difference of counters the allocators already keep), so leaving it in
cannot change the thing it measures. On a 32 MB machine that property is
worth more than the handful of cycles it costs.

The overlay draws in the same "after every 3D kick" packet as the uGUI
canvas rather than the clear packet, because the M8 debug overlay rides the
clear and the scene draws over it. A stats readout you cannot read is not a
stats readout.

## Two things the first on-target run corrected

Both are the argument for running the instrument rather than reasoning about
the numbers.

**The vsync wait was being counted as rendering.** The render zone read as
98 percent of the frame until `present()` got a zone of its own, at which
point 13.1 ms of that turned out to be the blocking vsync wait. A profile
that counts idle as work sends someone to optimise a frame that was already
finishing early. Present is now always its own zone.

**A vsync-locked frame is 33.37 ms, not 33.3.** Comparing against a round
33.34 reported 116 of 120 healthy frames as over budget. The question worth
asking is whether a flip was MISSED, which costs a whole field, so the
threshold sits at 40 ms: between one period and two, immune to a fraction of
a percent of clock calibration error, and still catching every genuinely
dropped frame.

## What the measurement said about the candidate list

Measured on a 114-entity scene with a skinned character, 120 frames on
target. Frame 33.62 ms, 29.74 fps, 1 dropped frame.

| Candidate (plan order) | Measured | Action |
|---|---|---|
| Scratchpad for DMA chain assembly | Chain build 0.019 ms | No headroom. Not done, and the reason is recorded rather than the work being quietly skipped |
| MMI/SIMD for transform update and culling | World matrices 0.157 ms; cull+queue **16.446 ms** | The cull number was 4x its entire section 15.3 budget and half the frame. Fixed, but not with SIMD (below) |
| Reduce managed/native interop | Already one dispatch per frame since M7, measured 87x cheaper than per-object | Already landed |
| GC tuning | 0 pauses reported, because nothing was reporting them | The histogram had no data source. Hooked; see below |
| Texture budget and upload scheduling | VRAM peak 4048 KB of 4096 (98 percent), textures skipped | 168 KB recovered by packing CLUTs; see below |
| Disc layout from an access trace | Not measurable in the emulator (host reads have no seek cost) | Blocked on hardware |

## The cull result, and why it was not a SIMD problem

The plan expected culling to want vector intrinsics. It did not. The
render queue's radix sort used 16-bit digits: four passes over a
65,536-entry histogram, each clearing it and prefix-summing it, which is
about 786,000 iterations across 256 KB of table **regardless of how many
commands are queued**. For 114 commands, on a machine with an 8 KB data
cache, nearly every one of those touches was a miss.

Two things were wrong and neither was arithmetic width: the cost did not
scale with the work, and the working set did not fit in cache. Switching to
256-entry digits makes the fixed cost 8 x 512 instead of 4 x 131,072 and the
table fits in cache; skipping any pass whose digit is uniform across the set
removes most of the remaining passes, because the high bytes of a render key
are identical for every command in an ordinary scene.

Measured on target, same scene, before and after:

| | Before | After |
|---|---|---|
| cull + queue | 16.446 ms | 0.352 ms |
| worst frame | 64.95 ms | 33.75 ms |
| dropped frames | 1 of 120 | 0 of 120 |
| fps | 29.74 | 29.99 |
| EE idle in present | 13.139 ms | 28.954 ms |

The frame time barely moved, because it was vsync-locked before and after.
What changed is that 16 ms of EE time moved out of culling and into idle:
the machine went from 60 percent occupied to 87 percent free. That headroom
is the deliverable, not the frame rate.

Writing the loop in MMI would have optimised the 0.3 ms that remained after
the real problem was gone, which is the exact failure mode the "profile
first" rule exists to prevent.

## The texture result: a page-granular allocator holding 1 KB objects

Video memory is allocated by 8 KB page, because FRAME.FBP and ZBUF.ZBP are
expressed in page units. A 256-entry CLUT is 1 KB. Every texture was taking a
whole page for its palette and wasting seven eighths of it: with two dozen
textures, about 168 KB of a texture pool that only has around 1.3 MB.

That is the entire reason scenes reported textures skipped for want of VRAM
while the allocator showed 98 percent occupancy. The occupancy was real; most
of what it was holding was nothing.

TEX0.CBP addresses a CLUT by 256-byte block, not by page, so eight palettes
fit in one page with no loss of addressability. Measured on target, same
scene: 24 CLUTs now occupy 3 pages (24 KB) where they previously took 24
pages (192 KB), and every texture in the scene is resident where two were
being dropped before. All five rendering goldens stay bit-identical, which is
the check that matters here, because a mis-addressed CLUT would draw with the
wrong palette rather than fail.

The general lesson is worth keeping: an allocator whose granularity is set by
one client's hardware constraint will quietly waste most of its space on a
different client whose objects are smaller than the grain.

## The GC result: an instrument with nothing wired to it

Every profile reported zero collector pauses, which reads as "the collector
never ran" and is indistinguishable from "nothing is measuring it". It was the
second. The histogram existed; no code called into it.

bdwgc reports progress through a collection, and for a stop-the-world
collector the span from GC_EVENT_START to GC_EVENT_END is the pause the frame
actually felt. The host now installs that callback immediately after
il2cpp_init, declared as a weak symbol rather than by including the
collector's headers, so a build linked without bdwgc still links and simply
records nothing. Both halves of that were verified against the EE toolchain:
it compiles, and it links with the symbol absent.

Tuning follows the measurement, not the other way round, so the tuning levers
in the plan's list (pre-allocation, pooling in the shim, incremental mode)
stay open until a managed workload produces pauses to tune against. What has
changed is that the number is now real instead of a zero nobody could
interpret.

## Consequences

- Every build carries the instrument, so a regression is measurable the day
  it lands rather than at the next milestone.
- Optimisations that were not done have a recorded number saying why, which
  is what makes "we chose not to" different from "we forgot".
- Skipping radix passes makes the number of buffer swaps data-dependent, so
  the sort now copies back when the count is odd. Three tests cover that
  parity directly, because a half-sorted queue would only appear for some
  inputs and would be miserable to find later.
