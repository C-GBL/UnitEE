# Third-party notices

Required by plan section 17.4 ("Track them in `THIRD_PARTY_NOTICES.md` and
honour attribution"). This file is engineering bookkeeping, **not legal
advice**; plan section 17 requires review by an actual lawyer before any public
release.

## What this repository contains

Only our own source. No third-party source is vendored here. In particular:

- **Unity's `libil2cpp`, `external/bdwgc`, and the `unityaot` BCL are never
  committed.** They are copied out of the user's own licensed Unity install at
  build time by `il2cpp-port/apply.py` into `build/`, which is gitignored and
  enforced by the pre-commit hook (`tools/git-hooks/pre-commit`). We ship only
  patches -- our own expression of changes -- plus new files we author.
- The ps2dev toolchain and PS2SDK are installed separately by
  `tools/ps2dev/install.ps1`; nothing from them is committed.
- No PS2 BIOS image is included, referenced, or downloaded by any script here.
  Booting anything in an emulator requires a dump the user makes from hardware
  they own.

## Components used at build or run time

| Component | Role | Licence | Notes |
|---|---|---|---|
| Unity Editor, `libil2cpp`, `unityaot` BCL | AOT compiler and CLR that user C# runs on | Unity's proprietary Editor terms | Each user builds with their own licensed install. Output binaries contain Unity-derived code exactly as a normal player build does; whether shipping that for an unsupported platform is permitted is the question for counsel (plan 17.1). |
| PS2SDK | EE/IOP headers, libc glue, `libgraph`/`libdraw`/`libpacket`/`libdma`/`libkernel` | Academic Free License 3.0 (post-2019 contributions) | Clean-room SDK. Deliberately used INSTEAD of Sony's official SDK. |
| ps2dev toolchain (GCC 15.2.0, binutils 2.45.1, `dvp-as`) | EE/IOP/VU compilers and assembler | GPL / GPL-with-runtime-exception | A compiler, not a linked dependency; the runtime exception covers normal compiled output. |
| gsKit | Reference for GS register setup only | Its own licence (see the gsKit tree) | ADR-003: reference and early bring-up only, NOT a runtime dependency. |
| bdwgc (Boehm GC) | Garbage collector for the managed heap | MIT-style permissive | Plan 17.5: for anything distributed, use the **upstream** bdwgc, not Unity's copy. |
| MinGW-w64 runtime DLLs (`libgcc`, `libstdc++`, `libwinpthread`, `libiconv`, `libgmp`, `libmpfr`, `libmpc`, `libisl`, `libexpat`, `liblzma`, `libtermcap`, `libzstd`) | Required to run the ps2dev Windows binaries | GPL-with-runtime-exception, LGPL, and permissive (varies per component) | Fetched from the MSYS2 mingw32 repo by `install.ps1` into the toolchain install, never committed. Host build tooling only -- they do not end up in a PS2 binary. |
| GoogleTest | Host unit tests | BSD-3-Clause | Fetched at configure time; test-only, never shipped. |
| PCSX2 | Development emulator | GPL-3.0 | External tool, invoked only. |
| `ps2-packer`, `ps2client` | ELF compression, host transfer | See ps2dev distribution | External tools, invoked only. |
| `mkps2iso` | ISO authoring with LBA control | See its repository | Not yet installed; needed from M12. |

## Trademarks

"PlayStation" and "PS2" are trademarks of Sony Interactive Entertainment.
"Unity" is a trademark of Unity Technologies. This project is not affiliated
with, endorsed by, or sponsored by either. Plan section 17.6 recommends naming
the shipped project so it implies neither -- the current `unity-ps2` /
`com.example.ps2` naming is a working placeholder and must be revisited before
any public release.

Do not use Sony's logos or boot logo, do not embed or distribute BIOS images or
Sony's official SDK, and do not implement disc-authentication circumvention
(plan section 17.3).
