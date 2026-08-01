# il2cpp-port

The layer that turns Unity's shipped IL2CPP runtime sources into a tree we
can compile for the PS2 Emotion Engine (`mips64r5900el-ps2-elf-g++`).

## Hard rule (plan section 8)

This directory contains ONLY patches and our own new files:

| Path | Contents |
|---|---|
| `apply.py` | staging tool: copies Unity sources into `build/il2cpp`, patches, overlays |
| `patches/` | `NNN-description.patch` unified diffs, applied in lexical order |
| `os/ps2/` | our new `IL2CPP_TARGET_PS2` OS-layer files, overlaid into the copy |
| `bdwgc/` | GC port reference material (ADR-004), not yet a patch |

Unity's `libil2cpp` and `bdwgc` sources are copied from the local Editor
install into `build/il2cpp/` (gitignored) at build time and are **never
committed** to this repository. The pre-commit hook that enforces this is
specified in a missing plan section: TODO(spec missing: section 17).

## Workflow

```
python il2cpp-port/apply.py prepare   # wipe + copy + snapshot + patch + overlay
python il2cpp-port/apply.py check     # exit 0 if build/il2cpp matches the prepared snapshot
python il2cpp-port/apply.py clean     # remove build/il2cpp
```

`prepare` is the default subcommand and is idempotent (it wipes and redoes
`build/il2cpp` every time). To modify a Unity source file, edit it under
`build/il2cpp` and capture the edit as a patch -- exact commands in
`patches/README.md`.

## Verified local paths (Unity 6000.0.47f1, this machine)

| What | Path |
|---|---|
| il2cpp AOT compiler | `C:/Program Files/Unity/Hub/Editor/6000.0.47f1/Editor/Data/il2cpp/build/deploy/il2cpp.exe` |
| libil2cpp source | `C:/Program Files/Unity/Hub/Editor/6000.0.47f1/Editor/Data/il2cpp/libil2cpp` |
| bdwgc source | `C:/Program Files/Unity/Hub/Editor/6000.0.47f1/Editor/Data/il2cpp/external/bdwgc` |
| AOT BCL | `C:/Program Files/Unity/Hub/Editor/6000.0.47f1/Editor/Data/MonoBleedingEdge/lib/mono/unityaot-win32` (unityaot-linux / unityaot-macos also present) |

Plan section 1.2 gives the compiler path as `build/deploy/net*/il2cpp[.exe]`
with a `[VERIFY the netX.0 folder name]` marker. **Resolved:** in 6000.0.47f1
the executable sits directly in `deploy/` -- there is no `net*` subfolder.

`apply.py` runs entirely on the host and does not need the PS2 toolchain.
The later native compile step consumes the staged tree via CMake and reaches
the toolchain through the `PS2DEV` environment variable (default
`C:/Users/Ash/ps2dev`, with `PS2SDK=$PS2DEV/ps2sdk`, `GSKIT=$PS2DEV/gsKit`).

## Open questions blocked on missing plan sections

- TODO(spec missing: section 11) -- the IL2CPP port spec: exact
  `IL2CPP_TARGET_PS2` wiring in `il2cpp-config.h`, the required `os/` class
  list and bring-up order, GC hook ownership, the double/soft-float policy
  (plan 3.1 consequence 1), managed stripping and `link.xml` policy.
- TODO(spec missing: section 9) -- milestone gates: M6 (null-GC bring-up,
  ADR-001 go/no-go on IL2CPP output size) and M10 (GC pause budget) drive
  ADR-004 follow-ups.
- TODO(spec missing: section 14) -- host (x86-64) build of the patched
  runtime for unit tests, which constrains how patches may be written.
- TODO(spec missing: section 17) -- legal boundaries and the pre-commit
  hook enforcing the hard rule above.
