# UnitEE

**Build PlayStation 2 games from Unity.**

UnitEE turns a Unity 6 project into a PlayStation 2 program. Scenes are exported
into native containers, your C# is compiled to MIPS through IL2CPP, geometry is
repacked for the Vector Units, textures are quantised into the 4 MB the Graphics
Synthesizer has, and the whole thing links into an ELF that boots.

Your MonoBehaviours run. No middleware, no engine fork.

- Website: [unitee.dev](https://unitee.dev)
- Licence: GPL-3.0
- Not affiliated with Unity Technologies or Sony Interactive Entertainment.

---

## Status

Milestones M0 through M12 are complete and verified in PCSX2. M12.5, which
closes the gap between components the runtime supports and components the
exporter carries, is in progress.

| Area | State | Measured on target |
|---|---|---|
| Geometry pipeline | Done | 14,976 triangles/frame, vsync locked |
| Managed code (IL2CPP) | Done | 1M Vector3 adds in 27 ms, worst GC pause 6.97 ms |
| Scene graph and rendering | Done | 502 entities, 5 material kinds, 29.91 fps |
| Animation and skinning | Done | 3 characters, 24 bones, crossfades, 29.97 fps |
| Physics and collision | Done | 71 us average step against a 4 ms budget |
| Platform services | Done | 24 audio voices, pads, memory card, streaming |
| Editor integration | Done | Unity scene to bootable ISO, 19.6 s clean build |
| Component coverage | Active | Closing the authored-to-exported gap |
| Profiler and hardware bring-up | Planned | |

Retail hardware is **not yet verified**. The development console for this
project failed mid-project, so hardware bring-up is its own milestone. PCSX2
forgives exactly what hardware does not, in particular DMA timing and cache
coherency after DMA.

---

## Quick start

### Requirements

| Component | Version |
|---|---|
| Unity Editor | 6000.0.47f1 |
| dotnet SDK | 9.0.304 |
| ps2dev toolchain | EE gcc 15.2.0, binutils 2.45.1 |
| PCSX2 | 2.6.3 (supply your own BIOS dump) |
| CMake, Ninja, Python | 3.10 or newer for Python |

### Install the toolchain

```
powershell -File tools/ps2dev/install.ps1
./tools/ps2dev/doctor.sh
```

The doctor script checks every compiler, assembler and library the build reaches
for, and reports what is missing.

### Add the package to a Unity project

Add a `file:` dependency to `Packages/manifest.json`:

```json
{
  "dependencies": {
    "com.example.ps2": "file:../../path/to/unity-package/com.example.ps2"
  }
}
```

Create a build profile (Create > Build Profiles > PlayStation 2), add your
scenes, and press Build in Window > PS2 > Build Profiles.

### Build headlessly

```
Unity -batchmode -quit -projectPath "path/to/YourProject" \
  -executeMethod Ps2.Editor.PS2BuildPipeline.BuildFromCommandLine \
  -ps2Profile Assets/Settings/PS2/Release.asset \
  -ps2Output Builds/PS2
```

The output is `game.elf` plus `game.iso`.

---

## How it works

The Editor side is a compiler, not a player. Nothing of Unity's own runtime
crosses onto the console. What crosses is data in UnitEE's formats plus C++ that
IL2CPP produced from your own source files.

**Stage 1, Unity Editor.** A build profile drives an eight step pipeline. The
scene exporter writes entities, meshes, materials, textures, skeletons, clips,
controllers, audio, fonts and baked collision into `.p2b` containers. Your
scripts are recompiled with Roslyn against `PS2.UnityShim`, the facade that
replaces `UnityEngine.dll`, and handed to IL2CPP.

**Stage 2, native build.** The generated C++, a patched build-time copy of
`libil2cpp`, the bdwgc collector and the `ps2ur` runtime compile and link against
PS2SDK into one ELF. Vector Unit microprograms are assembled by `dvp-as` and
linked in as data. `mkps2iso` wraps the result into a bootable ISO, controlling
file order on the disc.

**Stage 3, on target.** The runtime loads the container, starts the managed
world, and runs a frame loop of input, script `Update`, animation, physics,
culling, VU1 drawing and audio.

---

## What is supported

A constrained but genuine Unity subset, enforced by an Editor-side validator that
fails the build with actionable errors rather than producing a broken ISO.

- **Scripting:** C# as IL2CPP supports it, the GameObject and Component object
  model, the standard lifecycle, coroutines.
- **Rendering:** MeshRenderer, SkinnedMeshRenderer, Camera, directional lights,
  and a fixed material model. There are no shaders on a Graphics Synthesizer, so
  materials map onto register configurations.
- **Animation:** baked state machines, crossfades, 1D blend trees, additive and
  masked layers, root motion, matrix palette skinning.
- **Physics:** raycasts, sweeps, triggers, Rigidbody and CharacterController
  against a baked BVH. Deliberately not PhysX.
- **Platform:** 24 audio voices with streamed music, both controller ports with
  pressure and rumble, memory card persistence, additive and async scene loading.
- **UI:** a uGUI subset with layout baked at export and D-pad navigation in place
  of the pointer.

Every difference from Unity's semantics is a numbered conformance deviation in
[docs/supported-api.md](docs/supported-api.md), asserted by tests so it cannot
change silently.

---

## Building and testing the engine

The runtime compiles for both the console and the host, so most of it is
testable without a PlayStation 2.

```
# Host build: unit tests for allocators, formats, math, animation, physics.
cmake --preset host-debug
cmake --build --preset host-debug
ctest --preset host-debug

# Console build.
cmake --preset ps2-release
cmake --build --preset ps2-release

# Boot a sample headlessly and assert a token in the emulator log.
./tools/ci/run-emu-test.sh /abs/path/to/sample.elf PS2UR_TOKEN_OK
```

### Verification

Nothing here is signed off by looking at a screen.

- The runtime reads the framebuffer back off the Graphics Synthesizer and
  CRC-checks it in tiles against golden images. A change that alters one tile
  fails the check and names the tile.
- Managed milestones assert tokens parsed out of the emulator console log.
- 295 host tests plus 18 for the disc layout planner run on every change.

---

## Repository layout

| Path | Contents |
|---|---|
| `runtime/` | `ps2ur`, the console-side engine (EE, VU1, GS, IOP) with a host platform layer |
| `managed/PS2.UnityShim/` | The Unity-shaped API your scripts compile against |
| `unity-package/` | The Unity package: build profile, exporters, validation, toolchain invocation |
| `il2cpp-port/` | Patches and OS-layer files that add a PS2 target to a build-time copy of libil2cpp |
| `samples/` | Runnable samples, from a first triangle to a physics-driven kart |
| `tools/` | Toolchain pinning, CMake toolchain files, binding generator, golden tooling, CI glue |
| `docs/` | Architecture, supported API, formats, decision records, verification log |

---

## Documentation

| Document | Contents |
|---|---|
| [docs/supported-api.md](docs/supported-api.md) | Supported API, material model, conformance deviations |
| [docs/performance-guide.md](docs/performance-guide.md) | Performance envelope and hardware constraints |
| [docs/development.md](docs/development.md) | Development guide: commands, conventions, verified environment |
| [docs/formats/](docs/formats/) | Container, mesh and texture format specifications |
| [docs/adr/](docs/adr/) | Architecture decision records |
| [docs/notes/verify-log.md](docs/notes/verify-log.md) | Every trap hit on the way here, and how it was resolved |

Documents throughout the repository cite "plan section N". Those refer to the
internal engineering plan the project is built against, which is kept outside
this repository. The numbering is preserved in the citations so the reasoning
behind a decision stays traceable.

---

## Why this is possible

Unity's engine runtime is not source-available, and genuinely porting Unity to
the PlayStation 2 would need a platform-partner source licence that will never be
granted for a console from the year 2000. What this project ships instead rests
on three things Unity publicly provides:

1. `il2cpp` is a standalone AOT compiler. Given .NET assemblies it emits portable
   C++ that any C++ compiler can build, including the PS2 toolchain.
2. `libil2cpp`, the runtime that generated C++ executes against, ships as source
   with the Editor and has an explicitly abstracted platform layer designed for
   adding targets.
3. The Editor is fully scriptable at build time, giving complete access to
   project data for export.

So the deliverable is a translation layer: a compiler and exporter that turns a
Unity project into a native PlayStation 2 program, plus a small runtime that
presents a Unity-shaped API to the translated scripts.

---

## Contributing

Contributions are welcome, and roughly half the work is on the Unity side:
exporters, editor tooling, validation passes and documentation. The Vector Unit
side is smaller than it sounds, since each microprogram is one or two hundred
instructions.

Before starting, read [docs/development.md](docs/development.md) for the
conventions and the hard rules. In particular, build output and any copy of
Unity's `libil2cpp` or base class library must never be committed. A pre-commit
hook enforces this:

```
powershell -File tools/git-hooks/install.ps1
```

---

## Licence and notices

Released under the GNU General Public License v3.0. See [LICENSE](LICENSE).

No third-party source is vendored in this repository. Unity's `libil2cpp`,
bdwgc and base class library are fetched from your own Editor installation at
build time and are never redistributed here. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

PlayStation and PlayStation 2 are trademarks of Sony Interactive Entertainment
Inc. Unity is a trademark of Unity Technologies. This project is not affiliated
with, endorsed by, or supported by either company.
