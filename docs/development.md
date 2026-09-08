# Development guide

Working on UnitEE itself: the toolchains involved, the commands, the rules that
are not negotiable, and the environment details that cost real time when they
are wrong.

The authoritative engineering plan (sections 1 to 18) is an internal document
kept outside this repository, and is never modified. Where this guide and the
plan disagree, the plan wins. Citations of the form "plan section N" throughout
the documentation refer to it.

## Three toolchains

| Toolchain | Builds | Notes |
|---|---|---|
| ps2dev (MIPS) | The console runtime and samples | EE and IOP compilers plus `dvp-as` for Vector Unit microcode |
| CMake host (x86-64) | The same runtime for unit tests | Most of the engine is testable without a console |
| Unity and .NET | The Editor package and the managed facade | Unity 6 Editor plus the dotnet SDK |

All native code must build for **both** the console and the host. Anything
platform specific goes behind `runtime/src/platform/{ps2,host}/`.

## Commands

On Windows, run the `.sh` scripts under Git Bash. If your checkout path contains
a space, quote every path you pass by hand.

```
# Verify the toolchain. Run this first whenever anything behaves oddly.
./tools/ps2dev/doctor.sh

# Console build.
cmake --preset ps2-release && cmake --build --preset ps2-release

# Host build and unit tests.
cmake --preset host-debug && cmake --build --preset host-debug && ctest --preset host-debug

# Boot an ELF in PCSX2 headlessly and parse the console log for a token.
./tools/ci/run-emu-test.sh <elf> [token]

# Managed assemblies. Optional by hand: the Editor pipeline runs this itself
# when PS2.UnityShim.dll is missing (PS2BuildSteps.BuildShimAssembly).
dotnet build managed/PS2.Managed.sln -c Release

# Refresh the patched libil2cpp copy under build/.
python il2cpp-port/apply.py

# Install or repair the ps2dev toolchain.
powershell -File tools/ps2dev/install.ps1

# Install the pre-commit hook. Once per clone.
powershell -File tools/git-hooks/install.ps1
```

## Hard rules

- **Never commit anything under `build/`**, or any copy of Unity's `libil2cpp`
  or base class library. See `docs/adr/` and plan section 17. The pre-commit
  hook enforces this.
- **Changes to libil2cpp go in `il2cpp-port/patches/` as `.patch` files only.**
  Do not reformat that code. Generate patches with `git diff --output=...`,
  never with PowerShell `>` redirection, which rewrites line endings and stops
  the patch applying.
- **Never add a public API to `PS2.UnityShim` that does not match Unity's real
  semantics exactly.** A member that exists but does nothing is worse than a
  compile error: the compile error is found in the Editor, the silent no-op is
  found on a console. See `docs/supported-api.md`.
- **No `double` in runtime code or the facade's hot paths.** Floats on the
  Emotion Engine are not IEEE 754 and doubles are software emulated at 20 to
  100 times the cost.
- **Every architectural choice gets a decision record** in `docs/adr/`.
- Items marked `[VERIFY]` in the plan depend on the exact Unity install and are
  re-checked on every upgrade. Resolved values live in `docs/notes/verify-log.md`.
- ASCII only in source and documentation.

## Conventions

- C++17, four spaces, `PS2UR_` macro prefix, `ps2ur::` namespace, no exceptions
  in runtime code.
- All GS and DMA structures are 16-byte aligned. Use the qword alignment default
  and `static_assert` the size.
- Tests are required for allocators, math, format readers and writers, animation
  sampling and physics queries.
- `.clang-format` applies only to code owned by this project, never to libil2cpp
  or generated output.

## Environment

Re-check these only on a Unity or toolchain upgrade. The full detail, and the
traps that cost real time, are in `docs/notes/verify-log.md`.

| Item | Value |
|---|---|
| Unity Editor | 6000.0.47f1, at `<Unity Hub>/Editor/6000.0.47f1/Editor/Data` |
| il2cpp compiler | `.../il2cpp/build/deploy/il2cpp.exe`, directly in `deploy/` with no `net*` subfolder |
| libil2cpp and bdwgc | `.../il2cpp/libil2cpp` and `.../il2cpp/external/bdwgc` |
| AOT base class library | `.../MonoBleedingEdge/lib/mono/unityaot-win32` |
| dotnet SDK | 9.0.304 |
| ps2dev | Located through the `PS2DEV` environment variable. EE gcc 15.2.0, IOP `mipsel-none-elf` 15.2.0, `dvp-as` and binutils 2.45.1 |
| PCSX2 | 2.6.3. The BIOS dump is supplied by the developer and never committed |

### Two traps worth knowing in advance

1. The ps2dev Windows archive ships **no MinGW runtime DLLs**. Without them
   every tool exits with code `-1073741515` and prints nothing at all, which
   reads as a broken build rather than a missing dependency. `install.ps1`
   fetches them.
2. PCSX2 **rejects an ELF path containing a space**, and a relative path makes
   it execute garbage at `pc=0x0`. `run-emu-test.sh` stages into a space-free
   directory and makes paths absolute before launching.

## Verification

Rendering is checked by assertion, not by looking at a screen. The runtime reads
the framebuffer back off the Graphics Synthesizer and CRC-checks it per tile
against golden images; `tools/goldens/check.sh` guards against regressions.
Managed milestones assert tokens parsed from the emulator console log. The host
suite carries 295 tests, plus 18 for the disc layout planner
(`python tools/disc/test_layout_planner.py`).

When a component spans layers, per-layer tests are not enough. A component can
be supported by every layer and still not exist, because the call between two
layers is missing. Anything that crosses the Editor, the container, the runtime
and the facade needs an on-target assertion that the component actually did
something.

## Project status

Milestone status is summarised in the [README](../README.md). The detailed
record of what was tried, what failed and why lives in
`docs/notes/verify-log.md`, which is worth reading before debugging anything
that looks like it has been seen before.
