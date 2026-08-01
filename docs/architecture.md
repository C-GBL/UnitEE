# Architecture

**The authoritative plan is `ps2port.txt` at the repository root.** This file
summarizes it and tracks what is missing from it. Where they disagree, the plan
wins.

## System architecture (plan section 6, in prose)

The system is a three-stage pipeline. The key insight to internalise: **the
Editor side is a compiler, not a player.** Nothing of Unity's runtime crosses
the boundary -- only data in our own formats and C++ produced by `il2cpp` from
the user's own source code.

### Stage 1: Unity Editor (C#)

A `PS2BuildProfile` asset drives `PS2BuildPipeline`, which fans out into three
exporters:

- **SceneExporter** walks open scenes and emits entities, transforms, and
  component data into `scene.p2b`.
- **ScriptCollector** gathers the user's `.cs` sources from asmdefs and
  recompiles them with Roslyn against `PS2.UnityShim` (our facade assembly,
  replacing `UnityEngine.dll`), producing `Assembly-CSharp.dll`. That assembly
  is fed to `il2cpp --convert-to-cpp`, which emits portable C++.
- **AssetExporter** converts meshes, textures, animations, and audio through
  offline converters: a tri-stripper, a VU1 batcher, a palettiser + swizzler,
  an animation quantiser, and an ADPCM encoder -- emitting `assets/*.p2b`.

### Stage 2: Native build (CMake + Ninja)

The il2cpp output (`il2cppOutput/*.cpp`), a patched copy of `libil2cpp`,
`bdwgc`, and the `ps2ur` runtime -- plus VU1 microprograms assembled by
`dvp-as` and IOP `.irx` modules -- are compiled and linked by
`mips64r5900el-ps2-elf-g++` into `game.elf`. `ps2-packer` compresses the ELF,
and `mkps2iso` (which controls file LBA order on disc, a real seek-time lever)
produces `game.iso`.

### Stage 3: On target (PS2 / PCSX2)

Boot -> `ps2ur::Init` -> load `scene.p2b` -> `il2cpp_init` -> instantiate the
managed behaviours -> frame loop { input, managed `Update`, animation, culling,
VU1 draw chain, audio feed } -> present.

## Component responsibilities

- `unity-package/` -- the UPM package: build profile asset + window, exporters,
  validation, toolchain invocation.
- `managed/PS2.UnityShim` -- the Unity-shaped API the user's scripts compile
  against; its native side is `extern "C"` P/Invoke targets in
  `runtime/src/bridge/` (see ADR-002).
- `il2cpp-port/` -- patches + our new OS-layer files that add
  `IL2CPP_TARGET_PS2` to a build-time copy of Unity's libil2cpp (never
  committed).
- `runtime/` -- "ps2ur", the PS2-side engine (EE/VU1/GS/IOP), with a host
  (x86-64) platform layer so it can be unit-tested off-target.
- `tools/` -- toolchain pinning, CMake toolchain files, the binding generator,
  golden-image tooling, CI glue.

## Open items: missing plan sections

`ps2port.txt` ends at section 8. The following sections are referenced
throughout the plan but are NOT on disk and must be authored (or recovered)
before the work they specify can proceed beyond stubs:

| Section | Topic | Blocking |
|---|---|---|
| 7.2 | (Presumably the "not supported" list; the plan jumps 7.1 -> 7.3) | supported-api.md completeness |
| 9 | Milestone plan M0-M14 with tasks/deliverables/acceptance criteria | all scheduling; M6/M10 gates referenced by ADRs |
| 10 | Asset/container format spec (p2b, mesh batch, texture) | docs/formats/*, exporter, loader |
| 11 | IL2CPP port spec (incl. 11.5 double-perf trap detail) | il2cpp-port/ |
| 12 | Managed shim spec (incl. 12.3 Roslyn pass, 12.4 bindgen) | managed/, tools/bindgen/ |
| 13 | Editor integration spec (incl. 13.5 validator) | unity-package/ |
| 14 | Testing spec (incl. 14.1 host build, 14.3 goldens) | runtime/tests/, tools/goldens/ |
| 15 | Budgets | performance-guide.md finalization |
| 16 | Risks | -- |
| 17 | Legal (referenced by the section 8 hard rule on libil2cpp) | -- |
| 18 | Claude Code operating manual | CLAUDE.md is a stand-in |

Stubs referencing these carry `TODO(spec missing: section N)` markers.
