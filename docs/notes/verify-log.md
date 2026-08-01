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
| 2026-07-31 | **PCSX2 rejects ELF paths containing a space** | `-elf "C:/.../Unity 2 PS2/.../x.elf"` fails with "Requested boot ELF ... does not exist" even though the file is there. Since this repo lives under `Unity 2 PS2`, that is the normal case. `tools/ci/run-emu-test.sh` stages the ELF into a space-free directory before booting. Consistent with plan 4.1's space-free `PS2DEV` requirement. |
| 2026-07-31 | **A relative ELF path makes PCSX2 execute garbage** | PCSX2 resolves a relative `-elf` argument against the `host:` root it derives from the ELF's own directory, fails to read it, and then runs from `pc=0x0`, flooding the log with TLB misses. It looks exactly like a guest crash. Always pass an absolute path. |
| 2026-07-31 | M0 acceptance (boot test) | PASS: `samples/00-hello-triangle` boots in PCSX2 and `PS2UR_TOKEN_HELLO_TRIANGLE_OK` appears in the log within 2 s, asserted by `tools/ci/run-emu-test.sh`. |
| 2026-07-31 | M0 task 4: ps2sdk samples link | PASS: `draw/cube`, `draw/teapot`, `graph`, `hello` all link against the installed SDK (`-ldraw -lgraph -lmath3d -lpacket -ldma`). Note this ps2sdk has no standalone `pad` sample; `graph`/`draw` cover the GS path the plan cares about. `make` is NOT installed on this machine, so the samples were linked with direct compiler invocations rather than their Makefiles. |

## GS bring-up (M2)

Two bugs cost most of a bring-up session. Both are silent: the GS accepts the
bad data and rasterises it, so neither produces an error you can grep for.

| Date | Finding | Detail |
|---|---|---|
| 2026-07-31 | **PACKED-mode data uses a different layout from the register's native one** | A GIF qword carries a register value in one of two incompatible layouts. In **A+D** mode (`add_ad`) the low 64 bits hold the register's documented native value. In **PACKED** mode with an explicit register list, each field sits in its own 32-bit lane: `XYZ2` is X`[15:0]`, Y`[47:32]`, Z`[95:64]`; `RGBAQ` is R`[7:0]`, G`[39:32]`, B`[71:64]`, A`[103:96]`. Feeding a native value into a PACKED slot made XYZ2's Y read the low half of the native Z field and RGBAQ's G read part of Q. Symptoms: huge distorted triangles, dark/wrong colours, and a full-screen clear sprite that degenerated to nothing (a black screen). Fixed by `gs_packed_xyz` / `gs_packed_rgbaq` / `gs_packed_st` / `gs_packed_uv`, pinned by tests in `runtime/tests/test_gfx.cpp`. **State registers go through `add_ad`; vertex data goes through the packed encoders.** |
| 2026-07-31 | **`dma_wait_fast()` hangs on a channel that has never transferred** | It waits on channel status that only becomes meaningful once a transfer has been issued. `submit_and_wait()` called it before its first send, so the drawing environment never reached the GS and XYOFFSET/SCISSOR kept power-on values -- everything was scissored away. Presents as a **black screen**, not as a locatable hang, because the stall happens before the GS sees a byte. ps2sdk's samples only ever use it to wait on a *previous* send. Correct order: `FlushCache(0)` -> `dma_channel_send_normal` -> `dma_wait_fast()`. |
| 2026-07-31 | Cache writeback before DMA is mandatory | The EE writes packets through its data cache; the DMAC reads physical RAM. Without `FlushCache(0)` the GIF consumes stale bytes and rasterises them as primitives. The alternative is an uncached (UCAB) packet buffer, which trades every EE write for the flush; revisit when M4 moves chain assembly into the scratchpad, which is not cached. |
| 2026-07-31 | Drawing environment must be submitted, not just built | Obvious in hindsight, but the same black screen: `FRAME`/`ZBUF`/`XYOFFSET`/`SCISSOR` sat in a packet that was reset before it was ever sent. |
| 2026-07-31 | `GsDevice::set_trace()` | The bring-up aid that actually located the DMA stall: it logs each submit step so a wedged GS path can be placed before/during/after the DMA instead of guessed at. Reach for it before re-reading the code. |

### M2 acceptance

| Date | Criterion (plan section 9, M2) | Result |
|---|---|---|
| 2026-08-01 | `samples/01-spinning-cube` renders correctly | PASS, visually confirmed. |
| 2026-08-01 | Framebuffer CRC matches a checked-in golden | PASS: 64 tile CRC32s over a 256x256 window, reproducible across independent runs, stored in `tools/goldens/data/01-spinning-cube.golden`. `tools/goldens/check.sh` diffs them and was negative-tested (corrupting two tiles produces a failure naming exactly those two). |
| 2026-08-01 | "Sustained 60 fps with vsync in PCSX2" | **DEVIATION: 29.978 fps, and that is the correct maximum.** At 512x448 **interlaced** NTSC the display refreshes 59.94 *fields* per second, which is 29.97 *frames* per second; the sample is vsync-locked at exactly that, so it is not dropping frames. 60 fps at this line count is not physically available. Reaching 60 would mean either a ~224-line progressive mode or rendering per-field, both of which halve vertical resolution. The plan's section 3.3 baseline explicitly chooses 512x448 interlaced, so the 30 fps target in section 3.6 ("Realistic targets ... at 30 fps") is the consistent one; the M2 wording appears to assume a different video mode. Recorded rather than silently "fixed". |
| 2026-08-01 | EE frame timer | COP0 Count (`mfc0 $9`) at half the 294.912 MHz core clock = 147.456 MHz, extended to 64 bits by wrap detection. It is exact provided it is sampled more than once per ~29 s wrap, which `time::update()` per frame guarantees. |

## Texture pipeline (M3)

| Date | Finding | Detail |
|---|---|---|
| 2026-08-01 | **Texture swizzling is done by the GS, not by the exporter** -- correction to plan section 3.3 | Section 3.3 says "textures must be swizzled offline by the exporter; doing it at runtime is a waste of EE cycles." Measured on target: that is wrong for the normal upload path. A host->local transfer (`BITBLTBUF`/`TRXPOS`/`TRXREG`/`TRXDIR=0`, GIF IMAGE mode) takes a **raster** rectangle and the GS transfer engine writes it into VRAM in correct block order **in hardware, for free**. A 128x64 PSMT8 test scored **32/8192** texels correct when pre-swizzled and **8192/8192** when uploaded raster. The exporter must therefore emit raster indexed data. Offline swizzling would only be needed for a path that writes VRAM directly and bypasses the transfer engine; no such path exists yet. The hand-written swizzle routines were **deleted** rather than kept -- their block/column tables did not match the GS, and known-wrong code that looks usable is a trap. |
| 2026-08-01 | **CSM1 CLUT reordering IS required** -- plan section 3.3 confirmed | Unlike texel data, the palette is read positionally and the transfer engine does not fix it up. The 256-entry CSM1 palette is stored with 32-entry blocks reordered: within each group of 8 blocks, blocks 1<->2 and 5<->6 swap. Verified by uploading a pre-reordered palette and getting 8192/8192 texels with correct colours. A 16-entry PSMT4 palette is linear. |
| 2026-08-01 | Diagnostic that made this quick | `samples/07-swizzle` distinguishes the two failure modes automatically: wrong colours that still appear **in** the palette mean indices are misplaced (swizzle), colours **absent** from the palette mean the CLUT order is wrong. It printed "indices misplaced: suspect the SWIZZLE order" on the first run, which is what pointed at the upload path rather than the palette. |
| 2026-08-01 | PS2 alpha is 0-128, not 0-255 | `0x80` is fully opaque. Any alpha channel from a PC image format must be rescaled or everything renders half-transparent. `alpha_to_ps2` / `alpha_from_ps2` in `gs_swizzle.h`, round-trip tested. |

| 2026-08-01 | M3 acceptance (cache under thrash) | PASS on target: 24 textures (48 pages) cycled through a 16-page budget for 30 frames, 24/24 cells still the correct colour, 0 failed binds, residency never over budget. |
| 2026-08-01 | **LRU is pessimal for a cyclic scan larger than the cache** | That run recorded **hits=0, misses=720**: drawing textures 0..23 in order through a budget that holds 8 means each one has been evicted by the time it comes round again. This is the textbook LRU worst case, not a bug -- but it means the cache alone cannot fix a working set that does not fit. The renderer must sort draws by texture so each one is bound once per frame, which is exactly what plan section 9 M8 task 4 specifies ("sort by pass, material kind, texture, depth"). Until that lands, expect miss counts to look alarming in multi-texture scenes. |
| 2026-08-01 | Uploads consume frame-packet space | Every cache miss appends its image payload to the frame packet: 512 qwords for a 128x64 PSMT8, 16384 for a 256x256 PSMCT32. A thrashing cache overflows `VideoConfig::packet_qwords`, surfacing as `bind()` returning false rather than a dropped upload. Documented on `TextureCache`. |

## VU toolchain (`dvp-as`)

| Date | Item (plan ref) | Result |
|---|---|---|
| 2026-07-31 | `.vsm` dialect | RESOLVED: dvp-as needs the **`.vu` directive** to enter VU mode; without it every `NOP NOP` line is rejected as "bad instruction". Comments use `;`. Each line is an UPPER and a LOWER instruction issued together. Reference source: `$PS2SDK/samples/draw/vu1/draw_3D.vsm`. |
| 2026-07-31 | dvp-as output embedding (was OPEN; plan 9 M1 task 3) | RESOLVED, and the plan's assumption was wrong. dvp-as emits a **directly linkable EE object** with `.vutext`/`.vudata`/`.vubss` sections and global symbols taken from the source. Assembling `draw_3D.vsm` gives `VU1Draw3D_CodeStart` at 0 and `VU1Draw3D_CodeEnd` at 0x180 (24 pairs x 8 bytes, aligned to 16). **No `ld -r -b binary` or `.incbin` step is needed** -- `tools/cmake/vu.cmake` assembles and links the object directly, and `.vsm` sources declare their own `<Name>_CodeStart`/`_CodeEnd`. This matches what `packet2_vif_add_micro_program(pkt, 0, &Start, &End)` expects. Verified end to end: `vu_smoke.vsm` -> `VuSmoke_CodeStart/_CodeEnd` present in `libps2ur.a`. |

## Still open

| Item (plan ref) | Status |
|---|---|
| bdwgc `gcconfig.h` PS2 constants (`il2cpp-port/bdwgc/`) | OPEN: plan 11.4 now specifies `ALIGNMENT 4`, `CPP_WORDSZ 32`, `DATASTART`/`DATAEND` from `_fdata`/`_end`, `STACKBOTTOM` captured in `main()`, `GC_NO_THREADS`. The draft header still says `ALIGNMENT 8` and must be reconciled against 11.4 at M6. |
| Offline builds | OPEN: `runtime/tests` fetches GoogleTest v1.14.0 over the network at configure time, and `install.ps1` fetches MSYS2 packages. Neither has a vendored/mirrored fallback. |
| `mkps2iso` | OPEN: not part of the ps2dev distribution and not installed. Needed from M12 (ISO packaging). `doctor.sh` reports it as an optional absence. |
| Docker image + CI (M0 tasks 1, 7) | OPEN: Docker is not available on this machine and there is no CI runner. `tools/ci/README.md` tracks it. The plan's note that emulator tests need an out-of-band BIOS still stands -- never download one. |
| Host SDL2 rasteriser stand-in (M1 task 4) | OPEN: deferred until M2 defines the `gfx::Device` interface it must stand in for. The rest of the host build (platform layer, allocators, math, tests) is done. |
| Section 7.2 | OPEN: section 7 jumps from 7.1 to 7.3 in `ps2port.txt`; the "not supported" list appears to be missing. Sections 9-18 are now present. |
