# [VERIFY] log

Resolved `[VERIFY]` items from `ps2port.txt`, checked against the local
install. Per plan convention (section 0), these depend on the exact Unity
version and MUST be re-checked on every Unity upgrade. Record new rows (do
not delete old ones) when values change.

## Unity side

| Date | Item (plan ref) | Result |
|---|---|---|
| 2026-07-31 | il2cpp compiler location (1.2, 4.2: `deploy/net*/il2cpp[.exe]`) | RESOLVED: `C:/Program Files/Unity/Hub/Editor/6000.0.47f1/Editor/Data/il2cpp/build/deploy/il2cpp.exe` -- directly in `deploy/`, there is NO `net*` subfolder in 6000.0.47f1 |
| 2026-07-31 | libil2cpp source present (4.2) | RESOLVED: `.../Editor/Data/il2cpp/libil2cpp` exists (826 files copied by `apply.py prepare`) |
| 2026-07-31 | bdwgc present (4.2) | RESOLVED: `.../Editor/Data/il2cpp/external/bdwgc` exists (179 files) |
| 2026-07-31 | unityaot BCL present (4.2: `unityaot-linux`) | RESOLVED: `.../MonoBleedingEdge/lib/mono/unityaot-win32` exists; `unityaot-linux` and `unityaot-macos` also present |
| 2026-07-31 | .NET SDK 8+ (4.2) | RESOLVED: dotnet SDK 9.0.304 on PATH |
| 2026-07-31 | Unity version pin (4.2 says 6000.3/6000.4 LTS) | RESOLVED: pinned to installed **6000.0.47f1** (deviation from the plan's suggested 6000.3 LTS; matches the user Unity project at `C:/Users/Ash/Unity2PS2-UnityProject`). 6000.3.3f1 is also installed but unused. |
| 2026-07-31 | `il2cpp --convert-to-cpp` works standalone (1.2, ADR-001 option B) | RESOLVED: **YES.** Converts a test assembly to portable C++ in ~5.7 s, exit 0. Two switches are hidden from `--help` but required: `--dotnetprofile` must be **fully qualified** (`unityaot-win32`; bare `unityaot` is rejected) and `mscorlib.dll` must be passed explicitly in `--assembly` (il2cpp does no reference probing). See `il2cpp-port/notes/smoke-test.md`. |

## PS2 toolchain (ps2dev Windows prebuilt)

Installed to `C:/Users/Ash/ps2dev` (`PS2DEV`), from
`ps2dev-windows-latest.tar.gz` (~274 MB, github `ps2dev/ps2dev` tag `latest`).

| Date | Item (plan ref) | Result |
|---|---|---|
| 2026-07-31 | ps2dev prebuilt for Windows (4.1) | RESOLVED: native Windows prebuilt exists and works; no WSL/Docker needed (neither is available on this machine) |
| 2026-07-31 | EE compiler (4.1 table) | RESOLVED: `mips64r5900el-ps2-elf-gcc` **15.2.0** at `$PS2DEV/ee/bin` |
| 2026-07-31 | Exact IOP compiler triple (4.1 table, was OPEN) | RESOLVED: **`mipsel-none-elf-gcc` 15.2.0** at `$PS2DEV/iop/bin` -- matches the plan's guess |
| 2026-07-31 | VU assembler (4.1 table) | RESOLVED: `dvp-as` = GNU assembler (GNU Binutils) **2.45.1** at `$PS2DEV/dvp/bin` |
| 2026-07-31 | ps2dev `.pc` files carry CI-baked paths (4.1, was OPEN) | RESOLVED: baked prefix is `D:/a/ps2dev/ps2dev/ps2dev` (GitHub Actions runner). Rewrote **45 of 47** `.pc` files to the real install path; the 2 unchanged carry no absolute prefix. `tools/ps2dev/install.ps1` does this automatically and is idempotent. |
| 2026-07-31 | EE startup layout / crt0 (needed by `tools/cmake/ps2-toolchain.cmake`) | RESOLVED: `$PS2SDK/ee/startup/` contains **only `linkfile`** -- there is **no `crt0.o` anywhere** in ps2sdk. The gcc driver supplies startup. Linking with `-T<linkfile> -Wl,-zmax-page-size=128` alone yields a bootable ELF, so `PS2_LINK_EXPLICIT_CRT0` stays **OFF**. |

### Gotchas that cost real time (both handled by `install.ps1`)

| Date | Finding | Detail |
|---|---|---|
| 2026-07-31 | **The tarball ships no MinGW runtime DLLs.** | Every tool dies instantly with exit `-1073741515` (`STATUS_DLL_NOT_FOUND`) and **no error message**. The binaries are **32-bit** (PE machine `0x14c`), so the DLLs must come from the MSYS2 **mingw32 (i686)** repo -- x86_64 builds load but fail. 12 DLLs are needed, read out of the PE import tables: `libexpat-1`, `libgcc_s_dw2-1`, `libgmp-10`, `libiconv-2`, `libisl-23`, `liblzma-5`, `libmpc-3`, `libmpfr-6`, `libstdc++-6`, `libtermcap-0`, `libwinpthread-1`, `libzstd`. They are copied into every directory containing an `.exe` so resolution never depends on PATH order. |
| 2026-07-31 | **`tar` exits 1 on extraction, benignly.** | Six POSIX symlinks under `ps2sdk/ports/bin` (`bunzip`, `bzcat`, `bzcmp`, `bzegrep`, `bzfgrep`, `bzless`) cannot be created by Windows tar. They are bzip2 shell wrappers nothing here uses; everything else extracts correctly. `install.ps1` treats a non-zero tar exit as non-fatal and relies on a layout probe instead. |

## Emulator and on-target verification

| Date | Item (plan ref) | Result |
|---|---|---|
| 2026-07-31 | PCSX2 CLI flags (4.1 table) | RESOLVED: PCSX2 **v2.6.3** portable at `C:/Users/Ash/pcsx2`. `-batch -nogui -fastboot -elf <path>` works as documented. |
| 2026-07-31 | PS2 BIOS | User-supplied dump at `C:/Users/Ash/Documents/PCSX2/bios` (full set). Active: `ps2-0220a-20060210.bin`. **Not in this repo and never to be committed.** |
| 2026-07-31 | Capturing EE `printf` from a homebrew ELF | RESOLVED: requires **`EnableEEConsole = true`** in `Documents/PCSX2/inis/PCSX2.ini` (default is `false`). Output then lands in `Documents/PCSX2/logs/emulog.txt`. An empty log with a running ELF is a config problem, not a silent runtime. |
| 2026-07-31 | End-to-end: CMake toolchain -> bootable ELF | RESOLVED: `ps2ur` cross-compiles to `libps2ur.a` (16 objects, EE), an executable links against it through `tools/cmake/ps2-toolchain.cmake`, and it **boots in PCSX2** printing both a plain `printf` and output routed through `ps2ur::log` -> PS2 `platform::log_sink`. The ADR-002 bridge symbol `ps2ur_debug_log` was reached from `main()` on target. |
| 2026-07-31 | ELF exit behaviour | OBSERVED: returning from `main()` hands control to the BIOS, which shows the **memory-card/disc browser**. Correct for a bare ELF, but a shipped title must never fall off the end of `main()` -- it parks in `SleepThread()` or returns to the loader deliberately. Recorded in `runtime/src/platform/ps2/platform_ps2.cpp`. |
| 2026-07-31 | Log sink contract (bug found on target) | `ps2ur::log_va` already formats `"[ps2ur:<level>] "` into the string before calling `platform::log_sink`. A sink that adds its own tag double-prefixes (`[ps2ur:DEBUG] [ps2ur:info ] ...`, with mismatched levels since the enum is `Debug=0, Info=1, Warn=2, Error=3`). **Sinks emit `message` verbatim; the `level` argument is for routing only.** |

## Still open

| Item (plan ref) | Status |
|---|---|
| `dvp-as` output embedding strategy (`tools/cmake/vu.cmake`) | OPEN: whether assembled VU microprograms need `objcopy`/`bin2c` post-processing to become symbol-addressable blobs. Blocked on the first real `.vsm` (plan section 9). |
| bdwgc `gcconfig.h` PS2 constants (`il2cpp-port/bdwgc/`) | OPEN: `ALIGNMENT` (8 vs 16 for `lq`/`sq`), `DATASTART`/`DATAEND` linker symbols, and `STACKBOTTOM` must be read off `$PS2SDK/ee/startup/linkfile` and the driver's startup. Draft only; not yet a patch. |
| Offline builds | OPEN: `runtime/tests` fetches GoogleTest v1.14.0 over the network at configure time, and `install.ps1` fetches MSYS2 packages. Neither has a vendored/mirrored fallback. |
| Missing plan sections | OPEN: sections **7.2 and 9-18** are absent from `ps2port.txt`. See `docs/architecture.md` for the table of what each blocks. |
