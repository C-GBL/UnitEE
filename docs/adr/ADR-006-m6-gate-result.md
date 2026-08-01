# ADR-006: M6 gate result -- IL2CPP (option B) is GO

Status: accepted, 2026-08-01.

## Context

ADR-001 chose IL2CPP AOT + a native runtime (option B), with a custom IL->C
compiler (option C) as the documented contingency, decided empirically at
the M6 gate. Plan section 9 defines the gate: hello and feature programs
running on the EE, hello ELF <= 6 MB, a 5,000-line synthetic project
<= 12 MB, 1M Vector3 adds <= 250 ms, and a GC cycle over a 2 MB live heap
<= 8 ms.

## Decision

Option B is confirmed. All five criteria pass on PCSX2 (hardware-equivalent
timing): features correct, 5.23 MB hello, 5.45 MB synthetic, 27 ms
Vector3, 6.97 ms worst GC cycle. Full numbers, the build configuration
that produced them, and the accepted reductions are in `docs/m6-report.md`.

Option C (custom IL compiler) is retired as a contingency. The M6/M7
architecture proceeds on libil2cpp + bdwgc + the `il2cpp-port/os/ps2`
layer.

## Consequences

- The 12 MB text budget from section 15 holds with room: the runtime+BCL
  floor is ~5.2 MB and 5,000 lines of script cost ~215 KB.
- The GC pause budget is tight (13% margin at exactly 2 MB live) and
  scales with live-set size: the shim must pool aggressively (section 15)
  and heap growth is a per-title budget item from day one.
- Doubles are 58x floats on the EE (measured); the existing hard rule
  against doubles in hot paths is now backed by a number.
- Two build-system invariants are load-bearing and documented in
  verify-log: `keep-eh.ld` must accompany `--gc-sections` (EH tables are
  orphan sections in the ps2sdk linkfile), and user-assembly TUs compile
  -O2 under an otherwise -Os image (17x Vector3 regression otherwise).
- The unityaot-win32 BCL's Win32 P/Invoke surface is closed statically via
  `FORCE_PINVOKE_*_INTERNAL` + `os/ps2/Win32PInvokeShims.cpp`; new
  DllImport libraries entering the image fail the link, which is the
  intended tripwire.
