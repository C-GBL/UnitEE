# Unity -> PlayStation 2 Build Target

*"Write your game in the Unity Editor using a constrained Unity-compatible API,
press Build, get a PS2 disc image."*

This repository builds a PS2 build profile for Unity 6 (6000.0.47f1): a Unity
Editor package with a one-click Build button, an exporter that converts scenes,
meshes, textures, animations, and audio into a PS2-native container (`.p2b`),
an IL2CPP backend port so user C# `MonoBehaviour`s execute as native MIPS code,
a managed facade assembly (`PS2.UnityShim`) providing a Unity-compatible API
surface, and a native runtime ("ps2ur") for the EE/VU1/GS/IOP that loads the
container and runs the scene.

The authoritative engineering plan is [`ps2port.txt`](ps2port.txt) at the repo
root. Note: the plan file currently ends at section 8; sections 9-18 are
referenced but missing (see [docs/architecture.md](docs/architecture.md) for
the open-items list).

## Feasibility, honestly (plan section 1)

Unity's engine runtime is not source-available; genuinely porting Unity to the
PS2 is impossible without a platform-partner source licence that will never be
granted for a 2000-era console. What IS possible, and what this project ships,
rests on three things Unity publicly ships:

1. `il2cpp` is a standalone AOT compiler: given .NET assemblies it emits
   portable C++ (`--convert-to-cpp` without `--compile-cpp`) that any C++
   compiler can build -- including `mips64r5900el-ps2-elf-g++`.
2. `libil2cpp` (the CLR the generated C++ runs on) ships as source with the
   Editor, with an explicitly abstracted platform layer designed for adding
   targets (`IL2CPP_TARGET_*` switches). Adding `IL2CPP_TARGET_PS2` is a
   supported-shaped operation, even if unsupported in policy.
3. The Editor is fully scriptable at build time, giving complete access to
   project data for export.

So the deliverable is a translation layer: a compiler-and-exporter that
translates a Unity project into a native PS2 program, plus a small runtime on
the PS2 side presenting a Unity-shaped API to the translated scripts. Nothing
of Unity's runtime crosses the boundary -- only data in our own formats and
C++ produced by il2cpp from the user's own source code.

Verdict: **feasible, with a hard scope boundary.**

## Scope contract, in brief (plan section 7)

A constrained but genuine Unity subset, enforced by an Editor-side validator
that fails the build with actionable errors rather than producing a broken ISO:

- Full C# (as IL2CPP supports it), core math types, the
  GameObject/Component/Transform/MonoBehaviour object model, the standard
  lifecycle including coroutines.
- Rendering via a fixed material model (7 material kinds mapped to VU1
  microprograms -- there are no shaders on a GS), MeshRenderer,
  SkinnedMeshRenderer, Camera, simple lights.
- Clip-based animation, simple physics (raycasts, simple rigidbody, primitive
  colliders, CharacterController), 2D/simple-3D audio, DualShock 2 input, an
  immediate-mode uGUI subset, memory-card persistence, scene management, and
  an Addressables-like async load from a build-ordered disc layout.
- Documented conformance deviations (notably: hardware `float` is not
  IEEE 754 -- no NaN/Inf, saturating; `double` is soft-float and 20-100x
  slower). Full list in [docs/supported-api.md](docs/supported-api.md).

## Documentation

- [docs/architecture.md](docs/architecture.md) -- system architecture, open items
- [docs/supported-api.md](docs/supported-api.md) -- supported API, material model, conformance deviations
- [docs/performance-guide.md](docs/performance-guide.md) -- performance envelope and hardware constraints
- [docs/formats/](docs/formats/) -- container/mesh/texture format specs (stubs; section 10 missing)
- [docs/adr/](docs/adr/) -- architecture decision records (from plan section 5)
- [docs/notes/verify-log.md](docs/notes/verify-log.md) -- resolved [VERIFY] items
- [CLAUDE.md](CLAUDE.md) -- agent operating manual, verified environment, build commands
