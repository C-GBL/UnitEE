# M6 report: IL2CPP on the Emotion Engine (the go/no-go gate)

Date: 2026-08-01. Environment: the verified install in CLAUDE.md (Unity
6000.0.47f1, il2cpp/UnityLinker from that install, unityaot-win32 BCL, EE
gcc 15.2.0, PCSX2 2.6.3 headless via `tools/ci/run-emu-test.sh`). Every
number below was printed by the program under test on the emulated EE and
parsed out of the PCSX2 console log -- nothing is a host-side estimate.

## Verdict: GO for ADR-001 option B (IL2CPP AOT + native runtime)

All five acceptance criteria from plan section 9 pass:

| Gate | Bound | Measured | Margin |
|---|---|---|---|
| Console.WriteLine on EE in PCSX2 log | works | works | - |
| Feature program correct (classes, generics, interfaces, delegates, List, Dictionary, string, try/catch/finally) | correct | `M6_GATE_FEATURES_OK`, exit 0 | - |
| Hello-world ELF, stripped | <= 6 MB | 5.23 MB | 13% |
| Synthetic 5,065-line project ELF, stripped | <= 12 MB | 5.45 MB | 2.2x |
| 1M float Vector3 adds | <= 250 ms | 27 ms | 9.2x |
| GC cycle, 2 MB live heap | <= 8 ms | 6.97 ms worst, 6.95 ms avg (10 cycles) | 13% |

## The build that passed

Managed pipeline (`il2cpp-port/build_m6.py`): csc against the unityaot-win32
BCL -> UnityLinker `--core-action=link --i18n=none --rule-set=aggressive` ->
`il2cpp.exe --convert-to-cpp --dotnetprofile=unityaot-win32`.

EE build (`il2cpp-port/CMakeLists.txt` against the staged tree from
`apply.py`): libil2cpp + generated C++ + bdwgc (`extra/gc.c` amalgam,
IL2CPP_GC_BOEHM=1) + `os/ps2/` platform layer + `main_ee.cpp` host program.
`-Os -ffunction-sections -Wl,--gc-sections` with two carve-outs, both
measured (below); `-fexceptions` everywhere, single-threaded
(IL2CPP_SUPPORT_THREADS=0), `FORCE_PINVOKE_*_INTERNAL` for every DllImport
library in the image.

## Size

| Image | text | data | bss | stripped ELF file |
|---|---|---|---|---|
| hello (bdwgc, final) | 5.12 MB | 111 KB | 540 KB | 5.23 MB |
| synthetic 5,065 lines (final) | 5.31 MB | 124 KB | 540 KB | 5.45 MB |

What moved size, in order applied:

| Step | hello text | note |
|---|---|---|
| -O2 baseline | 6.75 MB | first successful link |
| -Os | 5.66 MB | -16% |
| + --gc-sections (function-sections only) | 3.67-3.78 MB | **broken at runtime** -- see EH trap below |
| + keep-eh.ld | 5.05 MB | correct; EH tables are ~1.3 MB of the image |
| + bdwgc (replacing null GC) | 5.12 MB | +65 KB code, +494 KB bss (GC tables) |

- The fixed cost is the runtime+BCL: 5,065 lines of script code added only
  ~215 KB over hello. Extrapolating linearly, a 50k-line game fits ~7.5 MB,
  comfortably inside the 12 MB budget from section 15.
- `--emit-null-checks=false --enable-array-bounds-check=false
  --enable-divide-by-zero-check=false` changed nothing for this profile:
  null checks are already off in standalone conversion and all 1,063
  bounds-check sites live in shared-generic BCL code that ignores the
  switch (site counts identical). The flags stay in build_m6.py as the
  documented release configuration.
- Two measured carve-outs from `-Os --gc-sections`:
  1. `keep-eh.ld`: the ps2sdk linkfile predates exception-heavy binaries
     and leaves `.eh_frame`/`.gcc_except_table` as orphan sections; section
     GC breaks the FDE/LSDA chains and the first managed `throw` hangs (or,
     with -fdata-sections, TLB-misses). Pinning both keeps EH working while
     code GC still runs.
  2. User-assembly TUs compile -O2: -Os cost 17x on the Vector3 benchmark
     (27 ms -> 468 ms) by refusing to inline the hot struct calls. BCL and
     runtime stay -Os. This is the shape a real game build wants anyway:
     script code is hot, the BCL is baggage.

## Performance (PCSX2, COP0-derived timing, 294.912 MHz EE)

| Benchmark | Result |
|---|---|
| 1M Vector3 (3x float) adds | 27 ms |
| 1M float mul-adds | 54 ms |
| 1M double mul-adds | 3,137 ms -- **58x float** |
| 200k small-object allocations, bdwgc | 624 ms (~3.1 us/alloc) |
| 200k small-object allocations, null GC | 1,809 ms (~9 us/alloc; malloc-backed) |
| GC.Collect, 2 MB live, worst of 10 | 6,969 us |
| 10,000 allocations under 2 MB live: worst pause | 56 us; zero pauses > 1 ms |
| Managed heap after GC bench (2 MB live) | 4.02 MB (conservative-GC factor ~2x) |

- The double number validates the hard rule (CLAUDE.md, plan section 4):
  doubles are software-emulated and catastrophic; they must never enter
  runtime hot paths. It also produced a bonus data point: the same
  1M-iteration accumulation in float diverged from the double result by
  ~1% (520,517 vs 525,854) -- the EE's non-IEEE single-precision drift is
  real and measurable, exactly why plan section 12 forbids trusting
  cross-platform float determinism.
- The GC gate passes at 13% margin at exactly the budgeted heap. Pause
  scales with live set; section 15's pooling guidance stands. bdwgc's
  allocation fast path beat the null GC's malloc path 3x.

## What it took (the port surface, all in `il2cpp-port/`)

- `patches/`: GC selection overridable (0001), MIPS pointer size (0002),
  stacktrace switch (0003), int32/long ABI fixes, bdwgc `gcconfig.h` PS2
  stanza (see below). Everything else is additive files, not edits.
- `os/ps2/`: config (`il2cpp-config-platform.h`, baselib platform header)
  plus File (legacy fio; fileXio is invisible to PCSX2 host:), Directory,
  Time (COP0), Console, Locale/Encoding (invariant, UTF-8), Cryptography
  (xorshift64*, documented non-cryptographic), Memory/MarshalAlloc,
  Process/NativeMethods (single-process constants), LibraryLoader (no
  dynamic loading), baselib (semaphore as plain counter -- with one thread
  a blocked Acquire could only be self-deadlock), libatomic shims (plain
  ops are correct single-threaded, not an approximation), Win32 P/Invoke
  shims, bdwgc hooks (GET_MEM over memalign, runtime-captured
  STACKBOTTOM).
- The unityaot-win32 BCL P/Invokes kernel32/advapi32/BCrypt/user32/
  timezone APIs directly (the win32 flavor is compiled for Windows; there
  is no unityaot-linux on a Windows Editor install -- verify-log). IL2CPP's
  own `FORCE_PINVOKE_<lib>_INTERNAL` mechanism turns every such call into a
  direct link-time call into `os/ps2/Win32PInvokeShims.cpp`: a shim gap
  fails the link instead of throwing on target. The shims report honest
  absence (no console, no registry, UTC, invalid find-handles); BCrypt
  random is the same non-cryptographic generator as os::Cryptography.
- bdwgc: PS2 stanza in `gcconfig.h` (MACH MIPS / OS_TYPE PS2, ALIGNMENT 4,
  DATASTART/DATAEND from linkfile symbols, STACKBOTTOM captured in main()
  because the BIOS picks the boot sp, GET_MEM as HBLKSIZE-aligned memalign
  chunks, NO_CLOCK/NO_GETENV/DONT_USE_ATEXIT). Built as Unity builds it:
  the `extra/gc.c` amalgam with the `gc_wrapper.h` define set (GCJ
  descriptors on, threads off). Beware: bdwgc's machine detection defaults
  bare mips to IRIX5, which drags in UNIX_LIKE/MMAP paths -- the stanza
  must pre-empt that (verify-log has the symptom table).

## Known reductions, accepted for M6 and tracked

- Single-threaded (plan's own instruction: "single-threaded first").
  Thread/Timer BCL surface exists but nothing preempts; v2 maps to ps2sdk
  kernel threads (plan 11.3).
- Culture surface is invariant/en-US; i18n stripped (`--i18n=none`).
- `Environment.TickCount`/`DateTime` run off COP0 + a fixed 2026-01-01
  epoch; no RTC until the cdvd RPC is wired (M8+).
- Directory enumeration reports empty (PCSX2 host: HLE serves opens, not
  listings); File.Exists-style probes work.
- GetRandom is not cryptographically secure (no entropy source on a PS2).
- Stopwatch needs System.dll; benchmarks use DateTime ticks instead to
  keep the size numbers honest. Pulling System.dll in later is a size
  line-item, not a port problem.
- Allocation rate (~3 us) is fine for bring-up but argues for the pooling
  strategy in section 15 before gameplay-scale churn.

## Checkpoint log (the order plan section 9 prescribes)

1. Pre-flight: EE C++ exceptions across TU boundaries -- proven before
   anything else (deep throw, rethrow, catch-all).
2. Convert + compile + link: 252 runtime TUs + generated code; final gaps
   closed by the os/ps2 layer above (the last 123 undefined symbols are
   enumerated in verify-log with their resolution).
3. Null-GC hello: first `PS2UR_TOKEN_M6_OK`, then the feature program.
4. Benchmarks under null GC (isolating GC from everything else).
5. bdwgc swap: same features, same checksums, GC gate measured.
6. Synthetic project: built by `build_m6.py --synthetic`, boots, checksum
   deterministic across GC implementations (31,811).

## If it had failed

Escalation order stood ready (harder stripping -> overlays -> ADR-001
option C custom IL compiler). None needed. Decision recorded in
`docs/adr/ADR-006-m6-gate-result.md`.
