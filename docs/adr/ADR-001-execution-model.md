# ADR-001: How does the game's C# execute on the PS2?

Status: Accepted (plan section 5, ADR-001)

## Context

Unity's engine runtime is not source-available and the platform-partner path
is closed, so a real platform port is off the table (plan section 1.1). The
user's scripts are C#; they must run as native MIPS code on a 294 MHz R5900
with 32 MB of RAM.

## Options

- **A. Full Unity platform port** -- obtain Unity engine source, implement
  `PlatformDependent/PS2`, ship a real platform module. **Rejected.** Requires
  a commercial source licence Unity will not grant for this. Non-starter.
- **B. IL2CPP + reimplemented runtime** -- use `il2cpp` to AOT-compile user
  scripts to C++ for the EE; port `libil2cpp`; write our own engine runtime;
  scripts talk to it through a facade assembly. Uses only publicly shipped,
  documented-shaped Unity tooling. Gives real C# semantics, real generics,
  real BCL.
- **C. Custom C# subset compiler** -- our own IL->C transpiler with a minimal
  runtime (no GC, no reflection, structs-only). Rejected as primary;
  **retained as fallback** if the IL2CPP port proves its output cannot fit in
  32 MB (TODO(spec missing: section 11) holds the detail). Much smaller
  output, much worse language compatibility.
- **D. Script interpreter** -- ship a Lua/MicroPython/IL interpreter,
  transpile C# to it. Rejected. Loses the entire point (people want to write
  C#) and is slower on a 294 MHz CPU where interpretation overhead is fatal.

## Decision

**Chosen: B**, with **C retained as a documented contingency evaluated at the
M6 gate**. The M6 acceptance criteria exist specifically to make the go/no-go
on this decision early and empirical rather than late and emotional
(TODO(spec missing: section 9): M6 definition and acceptance criteria).

## Consequences

- We depend on `il2cpp --convert-to-cpp` (without `--compile-cpp`) emitting
  portable C++, and on patching a build-time copy of `libil2cpp` with an
  `IL2CPP_TARGET_PS2` platform layer (`il2cpp-port/`).
- IL2CPP internals are version-sensitive: Unity is pinned to 6000.0.47f1 and
  `[VERIFY]` items must be re-checked on upgrade.
- Code size and RAM footprint of IL2CPP output on a 32 MB machine is the
  existential risk this ADR carries; it is measured, not assumed, at M6.
- Fallback C would restart significant work; keeping the managed<->native
  boundary narrow (ADR-002) limits the blast radius.
