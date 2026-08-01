# ADR-004: Garbage collector

Status: Accepted (plan section 5, ADR-004)

## Context

IL2CPP's runtime uses bdwgc (Boehm) as its GC. The target has 32 MB of RAM, a
294 MHz CPU, a 33 ms frame budget, and a 4-6 MB managed heap target (plan
3.6). Stop-the-world pause times are the concern.

## Options

- **Port bdwgc (Boehm), which IL2CPP already uses.** **Chosen for v1.** It is
  a conservative collector that needs only: data-segment bounds, stack
  bottom, alignment, single-thread config. That is a `gcconfig.h` stanza, not
  a rewrite (lives in `il2cpp-port/bdwgc/`).
- **Custom precise mark-sweep over a fixed arena.** Deferred. Better for a
  30 fps frame budget (bdwgc's stop-the-world pauses on a 294 MHz CPU are a
  real risk) but a much larger change to `libil2cpp`'s GC interface.
  **Revisit at M10 if pause times exceed 6 ms**
  (TODO(spec missing: section 9): M10 definition).
- **Null GC (never collect).** Rejected for shipping, **useful as an M6
  bring-up step** to isolate GC issues from everything else.

## Decision

bdwgc for v1, configured for the EE via a `gcconfig.h` stanza; a null-GC mode
as the first bring-up configuration at M6; the precise-collector option kept
open behind the M10 pause-time gate (>6 ms triggers the revisit).

## Consequences

- bdwgc source, like libil2cpp, is copied from the Editor install
  (`.../il2cpp/external/bdwgc`) into `build/` at build time and never
  committed; `il2cpp-port/bdwgc/` holds only our stanza and patches.
- The EE port must provide correct data-segment bounds and stack-bottom
  discovery for the ps2sdk CRT, and be configured single-threaded.
- GC pause times must be instrumented from the start (conformance deviation
  5: deterministic pauses are not guaranteed; the profiler exposes them), or
  the M10 gate cannot be evaluated.
- Managed-code guidance (docs/performance-guide.md) must steer users toward
  allocation-light patterns; the 4 MB heap target is part of the contract.
