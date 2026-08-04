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


## VU1 pipeline (M4)

| Date | Finding | Detail |
|---|---|---|
| 2026-08-01 | **PATH1 route proven end to end** | `samples/09-vu1-path1` draws a quad through VU1 rather than PATH3: MPG upload -> VIF UNPACK -> MSCAL -> VU1 -> XGKICK -> GS. **12288/12288 pixels correct on the first run.** That retires the plumbing risk on the plan's highest-risk milestone (section 16, R4); what remains is microprogram maths, which is where the risk should sit. |
| 2026-08-01 | VIF/DMA plumbing uses ps2sdk `packet2` | `packet2_vif_add_micro_program` / `packet2_utils_vu_*` handle MPG, UNPACK and MSCAL. Deliberate division of labour: our own GIF packet builder stays ours because the renderer manipulates it every frame, but re-deriving a VIF command encoder that ps2sdk already ships working would add risk to M4 for no benefit. `packet2_add_data(packet, ptr, qword_count)` appends a block; `packet2_add_u128` takes a single `u128` value, not two halves. |
| 2026-08-01 | A microprogram must end with `nop[E]` **plus one more pair** | The E bit stops the VU after the FOLLOWING instruction pair, so the trailing pair is required rather than padding. Omitting it runs VU1 into whatever else is in micro memory. `vu::wait_idle()` bounds its spin on VIF1_STAT bit 2 and reports the likely cause, because a VU that never terminates otherwise hangs the run with no clue. |
| 2026-08-01 | **VF00 is the hardwired constant (0,0,0,1)** -- the M4 transform bug | Writing the viewport map as `MULA ACC, pos, scale` / `MADD out, VF00, offset` looks idiomatic and is wrong: MADD computes `ACC + (VF00 * offset)`, and since VF00's x/y/z are zero it multiplies the offset away in exactly the three components that matter. That silently dropped the +2048 GS origin bias, sending every vertex off-screen; it rendered as a **full-screen smear**, not as nothing. Use an explicit `MUL` then `ADD`. Caught by `samples/10-vu1-transform`, which predicts the rectangle's screen extent arithmetically and reported "rectangle x span 0..512 (predicted 128..384) -- transform is wrong, not just imprecise". |
| 2026-08-01 | VU1 output is naturally PACKED | `FTOI4` and `FTOI0` write four int32 lanes, and a qword's lanes sit at bits 0-31 / 32-63 / 64-95 / 96-127 -- which is exactly where PACKED XYZ2 wants X/Y/Z and PACKED RGBAQ wants R/G/B/A. So a transformed vertex needs no repacking before XGKICK. |
| 2026-08-01 | `FTOI4` scales Z too | `FTOI4` gives X and Y the 12.4 fixed point the GS wants, but it multiplies the whole vector by 16, including Z. Pre-divide the Z viewport scale and offset by 16 in the per-batch constants; it costs nothing per vertex. |
| 2026-08-01 | M4 task 2 verified | `samples/10-vu1-transform`: identity MVP, clip-space input, rectangle lands at exactly the predicted 128..384 span. VU1 now does the matrix transform, perspective divide, viewport map and GS fixed-point conversion that the EE did at M2. 36 instructions. |
| 2026-08-01 | **M4 throughput target MET in PCSX2** | `samples/11-vu1-throughput`: 468 batches x 32 triangles = **14976 tris/frame at 29.822 fps** (446,626 tris/s) through `vu_unlit`, 36 VU instructions. Crucially the run is **vsync-locked** -- 29.822 is essentially NTSC's 29.97 ceiling -- so VU1 is not the bottleneck, the display is, and there is headroom above 15k. Per plan 14.4 this gates RELATIVE change only; the hardware run decides M4. |
| 2026-08-01 | The 30 fps threshold trap, again | Testing `fps >= 30.0f` failed a run that was hitting every vsync, because NTSC is 29.97 frames/s and 30.0 is unreachable. Same shape as M2's "60 fps" wording. Throughput checks compare against **29.5**. |
| 2026-08-01 | Per-batch packet allocation dominates measurement | A naive `draw_batch` that does `packet2_create`/`packet2_free` per batch allocates ~468 times a frame and measures the allocator rather than the VU. `MicroProgram::init_batching` preallocates one packet and `packet2_reset`s it. The DMA wait also belongs BEFORE overwriting the packet, not after sending, so the EE builds batch N+1 while the DMAC walks N. |
| 2026-08-01 | Batch size is set by VU1 data memory | 16 KB = 1024 qwords; header takes 8 and the output GIF packet needs 1 + 2 per vertex. 96 vertices (32 triangles) fits with room for the double buffering M4 task 5 adds. |
| 2026-08-01 | **M4 COMPLETE -- acceptance parity PASS** | `samples/14-vu1-chain` renders the same rotated cube through the M2 immediate path and through BatchBuilder + DmaChain + vu_unlit, and compares readbacks: **522/65536 pixels differ (0.7%)** against a 5% tolerance, all attributable to the EE path truncating to whole pixels while the VU keeps 12.4 subpixels. Chain profiling (task 7): 1 batch, 81 qwords, build 2us / kick 22us / wait 1us. |
| 2026-08-01 | Custom VIF blocks abandoned for the SDK pattern | The first DmaChain embedded hand-rolled VIF codes (STCYCL/UNPACK/MSCAL) inside each ref'd block; the VIF consumed the stream without drawing and without an error. Rather than debug a blind VIF stream, batches now use the SDK-proven `packet2_utils_vu_add_unpack_data` (ref tag, codes riding in the tag via TTE) + `add_start_program` (FLUSH+MSCAL) -- and blocks became PURE DATA (tag qword, count qword, vertex qwords), which is a better M5 file format anyway: the loaded mesh is referenced zero-copy with no code generation at export time. The hand VIF encoders remain in gs_batch.h, pinned by tests, for a future custom path. |
| 2026-08-01 | **Sticky status flags accumulate from EVERY upper instruction** | The first clipping build rejected all geometry: FSSET..FSAND windows must contain ONLY the test SUBs, because half of all clip-space intermediates are negative and each one sets sticky S. All transform/lighting arithmetic first, then FSSET 0, three test SUBs, >=4 pairs, FSAND 0x80. Both microprograms carry the discipline in their header comments. |
| 2026-08-01 | Clipping verified (staged) | `samples/13-vu1-clip`: visible control 8173 px drawn; behind-camera, straddling and beyond-guard triangles 0 px each; 0 unexpected pixels. Straddling triangles POP (whole-triangle reject) rather than clip -- the staged behaviour the plan schedules first; true near-plane clipping is the recorded follow-up. |
| 2026-08-01 | DmaChain assembles in main RAM, not scratchpad | Deviation from M4 task 5 recorded in dma_chain.h: chain build is 2us/frame and throughput is vsync-locked, so scratchpad assembly is not currently the bottleneck; its profiling counters are how that decision gets revisited. |
| 2026-08-01 | `.vsm` blob size in instructions | dvp-as emits 64-bit instruction pairs, so `(CodeEnd - CodeStart) / 8` is the instruction count. `vu_passthrough` is 4 instructions (0x20 bytes). |


## Asset pipeline (M5)

| Date | Finding | Detail |
|---|---|---|
| 2026-08-01 | **M5 acceptance PASS** | A scene built and exported by Unity 6000.0.47f1 in batchmode (22 entities, 20 mesh instances over 15 deduplicated meshes, 11 materials, 10 quantised PSMT8 textures, camera, directional light; 208,896 bytes of .p2b) loads and renders on target through all three VU1 microprograms resident simultaneously (unlit @0, textured @300, lit @700). 31,395/65,536 verification-window pixels drawn; 64-tile golden captured and **bit-identical across independent runs**. Deviation recorded in the sample: the reference is a checked-in golden of this renderer, not an Editor-side constrained render (which does not exist yet). |
| 2026-08-01 | **PCSX2 `HostFs = false` is the default, and the failure is misleading** | With host filesystem access disabled, PCSX2 still logs "HLE Host: Set 'host:' root path" at boot -- but every open fails with fio error **-5**. Nothing suggests a config switch. `HostFs = true` in PCSX2.ini is required for `host:` file loading. (-19 = no such device, for comparison.) |
| 2026-08-01 | **ps2sdk newlib file I/O does not reach PCSX2's host:** | Modern ps2sdk routes `open()` exclusively through fileXio (`io_common.h` has a compile-time guard against direct fio use). Loading iomanX.irx + fileXio.irx (now embedded in the runtime via `.incbin` and loaded at `platform::init`) still does not make `host:` visible to that route under PCSX2's HLE. The runtime's `io::load_file` therefore uses LEGACY fio on the PS2 build, with its own extern declarations and its own fds (never mixed with newlib's, which is what the guard is about). Revisit on real hardware with ps2link. |
| 2026-08-01 | `SifInitRpc(0)` belongs in platform init | Everything IOP-side (file I/O, later audio/pads) rides the SIF; forgetting it makes fio fail with no useful error. The M0 sample called it explicitly, which is why printf worked before the runtime did. |
| 2026-08-01 | GS V axis vs Unity | Unity UV origin is bottom-left, GS texture space top-left: the exporter writes `1 - v`. Same for `GetPixels32` row order (bottom-up), which the texture exporter flips. |

## VU toolchain (`dvp-as`)

| Date | Item (plan ref) | Result |
|---|---|---|
| 2026-07-31 | `.vsm` dialect | RESOLVED: dvp-as needs the **`.vu` directive** to enter VU mode; without it every `NOP NOP` line is rejected as "bad instruction". Comments use `;`. Each line is an UPPER and a LOWER instruction issued together. Reference source: `$PS2SDK/samples/draw/vu1/draw_3D.vsm`. |
| 2026-07-31 | dvp-as output embedding (was OPEN; plan 9 M1 task 3) | RESOLVED, and the plan's assumption was wrong. dvp-as emits a **directly linkable EE object** with `.vutext`/`.vudata`/`.vubss` sections and global symbols taken from the source. Assembling `draw_3D.vsm` gives `VU1Draw3D_CodeStart` at 0 and `VU1Draw3D_CodeEnd` at 0x180 (24 pairs x 8 bytes, aligned to 16). **No `ld -r -b binary` or `.incbin` step is needed** -- `tools/cmake/vu.cmake` assembles and links the object directly, and `.vsm` sources declare their own `<Name>_CodeStart`/`_CodeEnd`. This matches what `packet2_vif_add_micro_program(pkt, 0, &Start, &End)` expects. Verified end to end: `vu_smoke.vsm` -> `VuSmoke_CodeStart/_CodeEnd` present in `libps2ur.a`. |

## IL2CPP port (M6)

Full gate numbers in `docs/m6-report.md`; decision in ADR-006. These are the
traps and resolutions.

| Date | Finding | Detail |
|---|---|---|
| 2026-08-01 | **M6 acceptance PASS -- all five gates** | Hello 5.23 MB / synthetic 5.45 MB stripped, Vector3 27 ms, GC 6.97 ms worst over 2 MB live, features correct. Every number parsed from the PCSX2 log, printed by the program under test. |
| 2026-08-01 | **unityaot-win32 BCL P/Invokes Win32 directly** | The only AOT BCL on a Windows Editor install is the win32 flavor: System.Console's cctor (and TimeZoneInfo, registry, BCrypt paths) carry `DllImport("kernel32.dll")` etc. compiled in -- `Environment.get_Platform` returning Unix does not help. Resolution: IL2CPP's generated code wraps every call site in `FORCE_PINVOKE_<lib>_INTERNAL`; defining those (kernel32 in both spellings, advapi32, BCrypt, api_ms_win_core_timezone_l1_1_0, user32) makes them direct extern calls into `os/ps2/Win32PInvokeShims.cpp`, resolved at link time. Do NOT force `libc`: its `snprintf` declaration collides with newlib's prototype in TUs that include stdio. |
| 2026-08-01 | **`File::Isatty` must be false on the EE** | Claiming the TTY is a terminal sends Mono's Console down the TermInfoDriver path (termcap files, TERM variable) and the type initializer throws. The EE kernel TTY is a write-only stream; "not a tty" selects plain streams and Console.WriteLine flows through MonoIO to the PCSX2 log. |
| 2026-08-01 | **bdwgc defaults bare MIPS to IRIX5** | gcconfig.h's machine detection (~line 216) has a legacy `mips => IRIX5 unless a known OS` default that fires before any later stanza can, dragging in `UNIX_LIKE`/`MMAP_SUPPORTED` (sigsetjmp, sys/mman.h -- neither exists on newlib/ps2sdk). The PS2 stanza must define `PS2` BEFORE that block and add `!defined(PS2)` to the IRIX5 guard. Probe with `-dM -E` on a file including gcconfig.h; `OS_TYPE "PS2"` and no `UNIX_LIKE` is the pass condition. |
| 2026-08-01 | **`--gc-sections` silently breaks C++ EH on ps2sdk** | The ps2sdk linkfile KEEPs `.ctors`/`.dtors` but leaves `.eh_frame`/`.gcc_except_table` as orphans; section GC breaks the FDE/LSDA chains. Symptom: everything runs until the first managed `throw`, then a hang (or TLB miss at a garbage address with `-fdata-sections`). Fix: `il2cpp-port/keep-eh.ld` (INSERT AFTER .dtors, KEEP both) passed as a second `-T`. EH tables are ~1.3 MB of the hello image. |
| 2026-08-01 | **-Os on user-assembly code costs 17x on Vector3** | 27 ms -> 468 ms: -Os refuses to inline the hot struct calls. Resolution: image-wide -Os, per-file -O2 on the user assembly's generated TUs (source-property appended last wins). BCL/runtime stay -Os. |
| 2026-08-01 | Interlocked and Math icalls live under `vm-utils/icalls/*/*/`, not `icalls/` | Three path levels; a two-level glob misses them and the symptom is ~20 undefined Interlocked symbols plus `Math::Pow` at link. `RuntimeImports::ecvt_s` is Windows-only upstream and needs a PS2 implementation (printf %e digit extraction). |
| 2026-08-01 | `g_CodegenRegistration` is guarded by `RUNTIME_IL2CPP` | The generated `Il2CppCodeRegistration.cpp` only emits the registration object under `#if RUNTIME_IL2CPP` -- define it on that one file. Without it, il2cpp_init finds no metadata and every lookup fails. |
| 2026-08-01 | gcc builtin `__atomic_*` shims must be spelled `unsigned int`/`unsigned long long` | `uint32_t` is `unsigned long` on the EE and "ambiguates built-in declaration". The out-of-line shims (no libatomic in ps2dev) are plain single-threaded ops -- correct, not approximate, with THREADS=0. |
| 2026-08-01 | Metadata/resources staging layout | `il2cpp_set_data_dir("host:")` makes the runtime open `host:/Metadata/global-metadata.dat` and `host:/Resources/mscorlib.dll-resources.dat` (satellite needed by culture/string paths; its absence turns into a TypeInitializationException far from the cause). Both staged under the emu stage dir by the M6 harness invocation. |
| 2026-08-01 | Stopwatch lives in System.dll | mscorlib-only images must time with `DateTime.UtcNow.Ticks` (100 ns, COP0-backed via `os::Time`). Pulling System.dll in is a size line-item when actually needed. |
| 2026-08-01 | bdwgc heap factor and alloc rate | 2 MB live -> 4.02 MB heap (conservative ~2x). bdwgc alloc ~3.1 us/object beat the null GC's malloc path (~9 us) 3x. GC worst pause 6.97 ms at exactly the budgeted live set -- pooling (plan 15) is not optional at gameplay scale. |

## Managed facade / bridge (M7)

| Date | Finding | Detail |
|---|---|---|
| 2026-08-01 | **M7 acceptance PASS** | Spin.cs written against real Unity, exported (script components in .p2b via the new SCRP section), recompiled untouched against PS2.UnityShim, runs on the EE: 300-frame localToWorldMatrix parity vs an Editor-recorded golden, max deviation 5.72e-05 (tolerance 5e-3, so ~100x margin) with the EE's non-IEEE floats. Cube renders and visibly spins (window CRCs differ across frames). |
| 2026-08-01 | **Dispatch cost: runtime_invoke 4.7 us/call, cached methodPointer 54 ns/call (87x)** | M7 task 4 measurement over 10k no-op calls on PCSX2. Per-object interop would burn milliseconds; the design (ONE managed Runtime.Tick per frame, fan-out managed-side) is load-bearing, not a style choice. il2cpp static-method direct ABI: (args..., const MethodInfo*). |
| 2026-08-01 | **User scripts must recompile into an assembly named Assembly-CSharp** | The exporter records "Full.Name, Assembly" from the Editor, where loose scripts live in Assembly-CSharp; Type.GetType on target must find the same string. build_m7.py names the recompiled game assembly accordingly. link.xml preserving Assembly-CSharp AND PS2.UnityShim is mandatory: CreateScript uses Type.GetType + Activator, lifecycle dispatch uses reflection-once-then-delegate. |
| 2026-08-01 | **A spinning cube can fool a naive did-it-move CRC** | Frames 450 deg apart are pixel-identical: a cube has 90-degree rotational symmetry and directional lighting maps onto itself. Sample frames for motion checks must differ by a NON-multiple of the object's symmetry angle. Cost one boot cycle. |
| 2026-08-01 | bindgen emitters live (12.4) | api-def -> C# externs + C prototypes + layout static_asserts + registration table + docs table; 'check' subcommand gates configure/CI on freshness. The registration table doubles as the linker-keep mechanism for --gc-sections builds. |
| 2026-08-01 | Editor golden generation is chicken-and-egg | SpinParityCheck references SpinGolden (generated BY the exporter): batchmode compile fails before the first export. A placeholder SpinGolden.cs with an empty array bootstraps; the exporter overwrites it. |

## Scene graph / renderer (M8)

| Date | Finding | Detail |
|---|---|---|
| 2026-08-01 | **M8 acceptance PASS** | samples/02-scene-graph: 502 entities (25 towers x 20 cubes, 20-deep parent chains), 5 material kinds incl. transparent/cutout/additive, moving camera over 300 frames at **29.91 fps** (NTSC ceiling 29.97; bar is 29.5), culling sweep drawn range [0,481], 8 fixed camera poses -> 512 golden tiles **bit-identical across runs**. Radix-sorted queue: opaque front-to-back, transparent back-to-front with Z write off. |
| 2026-08-01 | **Lighting scale bug hiding in plain sight since M5** | Sample 15 passed light colours *255 with ambient 40 while vertex colours are 0..255: everything lit saturated WHITE, and the golden pinned it -- deterministic-but-wrong survives golden tests when the golden is captured from the same wrong code. Ground truth was sample 12 (exact-arithmetic lighting): light factor ~0..1 x vertex colour 0..255. Fixed in scene_renderer + sample 15 + main_m7; goldens legitimately regenerated. Lesson recorded: a golden pins DETERMINISM, not CORRECTNESS -- correctness needs at least one arithmetic assertion like sample 12's. |
| 2026-08-01 | **Fog on target, cheap and exact** | vu_lit_fog computes f = clamp(w*scale+offset, 0, 255) per vertex; the FOG register descriptor (0xA) reads F from qword bits [107:100], which is precisely where FTOI4 lands an integer's bits [11:4] from the W lane -- so packing F costs 7 upper ops and no integer bit surgery. PRIM.FGE + FOGCOL do the rest. Verified monotonic on target: five cubes at increasing depth slide 328 -> 270 -> 178 -> 85 -> 0 L1-distance from FOGCOL, far cube (past fogEnd) EXACTLY the fog colour. Fog kind is chosen at export (RenderSettings.fog + VertexLit -> kind 6) because the GIF tag layout (nreg 3, regs 0x5A1) is baked into mesh blobs. |
| 2026-08-01 | VU0 macro-mode matrix multiply | mat4_mul on the EE now runs on COP2 (lqc2 + MULAx/MADDAy/MADDAz/MADDw per column). Mat4 gained alignas(16) -- lqc2 faults on unaligned. VU MADD rounds once where scalar mul+add rounds twice, so results differ from host at the last bit; goldens are per-platform and 01-spinning-cube's golden happened to survive unchanged. |
| 2026-08-01 | Radix-sort scratch is 256 KB | The 16-bit-digit histogram (65536 u32) lives in a static, NOT on a stack: EE threads default to 8 KB stacks and the overflow is silent corruption, not a fault. |
| 2026-08-01 | MATL v2 is a breaking format change | 48-byte records (TEST/ALPHA/flags as data). The reader REJECTS other strides instead of guessing; every shipped .p2b regenerates from the exporter in the same commit. Old-vs-new ambiguity (24 divides 48) is why stride sniffing was not attempted. |
| 2026-08-01 | Dirty-tracked world matrices | Setters mark dirty; the pass recomputes an entity when itself or an ancestor changed, resolves file order in one pass and reparent forward-references iteratively. With one moving camera in a 502-entity scene the pass multiplies 1 matrix, not 502. |
| 2026-08-01 | A did-it-move CRC needs symmetry awareness | (From M7, reconfirmed designing M8 goldens.) Frames of a spinning cube 450 degrees apart are pixel-identical (90-degree symmetry x directional light). Golden poses and motion probes must avoid the object's symmetry group. |

## Animation and skinning (M9)

| Date | Finding | Detail |
|---|---|---|
| 2026-08-01 | **M9 acceptance PASS** | samples/17-skinning: three characters (1,536 triangles, 24 bones, 96 batches each) animating with crossfades at **29.97 fps** -- the NTSC ceiling, i.e. vsync-locked. Golden POSE-MATRIX parity against Unity's own sampler over the 5-second clip: max element error **0.0188** across 30 samples x 24 bones (budget 0.02). Golden image: 64 tiles, bit-identical across runs. |
| 2026-08-01 | **The lighting constants were per-CHANNEL, but the microcode wants per-LIGHT** | vu_lit's MADD chain broadcasts N.L per light (x, then y, then z) and spends its fourth slot on ambient, so qwords 12..14 must hold the COLOURS of lights 0..2 as (r,g,b,0) -- three directional lights, not four. Every call site packed them as (R,0,0,0),(G,0,0,0),(B,0,0,0) instead, which lit ONLY THE RED CHANNEL: greens and blues got ambient alone. It survived from M4 to M9 because sample 12 -- the exact-arithmetic lighting test -- asserted `p[0]` and nothing else, and the M5/M8 goldens then pinned the wrong image. Sample 12 now uses a non-grey light (200,140,80) and asserts all three channels; measured exactly on target. **Lesson, again: a golden pins determinism, not correctness -- and an arithmetic test that checks one component of three is a third of a test.** |
| 2026-08-01 | **The VIF NUM field, not VU memory, caps a skinned batch** | Skinned vertices are 5 qwords (position, normal, colour, palette offsets, weights). VU1 data memory would hold 97 of them above the 24-matrix palette, but UNPACK's NUM field is 8 bits, so a batch tops out at 255 qwords = 51 vertices. 48 (16 triangles) is the usable figure, which is why a 1,500-triangle character is ~96 batches. The loader rejects `vert_qw > 255` explicitly rather than letting the count truncate silently on target. |
| 2026-08-01 | Blend the matrices, not the vertices | vu_skin computes M = sum(w_i * M_i) with 16 MADDs and then transforms position (4 ops) and normal (3), rather than transforming by each bone and blending four results (28 ops). The palette holds MODEL-space matrices (bone_world * inverse_bind) so the skinned normal stays in the object space the lighting block expects; the extra MVP multiply is cheaper than a second rotation-only palette. Documented limitation: normals use the blended matrix, not its inverse transpose, so non-uniform bone scale lights incorrectly. |
| 2026-08-01 | Weight renormalisation is not optional | Quantised weights that sum to 0.999 leave the blended matrix's w slightly off and the perspective divide drifts. The exporter renormalises, and the microprogram additionally pins w with `MULw.w VF29, VF00, VF00w` (VF00 is hardwired (0,0,0,1)). |
| 2026-08-01 | An exact invariant beats a golden for blending | The crossfade test asserts that adjacent bone origins stay exactly `rest_spacing` apart through the blend -- true for any rotation-only rig, independent of any reference. Measured drift 1e-06. A quaternion lerp without renormalisation, or a hierarchy composed in the wrong order, shrinks the chain immediately; a golden would have pinned that silently. |
| 2026-08-01 | A 0.5 s crossfade at 30 fps takes 15 OR 16 frames | 15 x (1/30) accumulates to a hair under 0.5 in float, so the blend clears on frame 16. The test asserts 14..17 frames and monotonicity rather than an exact count -- an exact count is a false precision that fails on a different dt. |
| 2026-08-01 | Comparing against a pose golden needs the clip sampled at t=0 | The first comparison must advance the animator by ZERO (which evaluates the pose without moving time), not read the rest pose: the clip is already non-identity at t=0, and the rest pose is off by 3.96 units. |
| 2026-08-01 | Keyframe-reduction tolerance drives pose parity, not quantisation | At a 0.57-degree reduction tolerance the accumulated tip error down a 24-bone chain was 0.249; at 0.05 degrees it is 0.019, of which 16-bit quantisation contributes ~3e-5 per bone. Reduction tolerance is the dial that matters for a long chain, and it must be set against the chain's lever arm, not per-bone intuition. |

## Audio (M10 task 1)

| Date | Finding | Detail |
|---|---|---|
| 2026-08-01 | **SifExecModuleBuffer silently fails to START audsrv** | It works for iomanX and fileXio, but for audsrv it returns a plausible-looking id (301312) with mod_res 0 while the module's _start NEVER RUNS -- no "greetings from version 0.93", no RegisterLibraryEntries -- and audsrv_init() then blocks forever on an RPC server that does not exist. PCSX2 shows it only as "[EE] Skipping timeout loop". Adding SifInitIopHeap(), a FlushCache(0) and 64-byte blob alignment did not help; the identical .irx loaded with SifLoadStartModule from a file works first time. The runtime now loads audsrv from host:/cdrom0:/mass: with the embedded blob as a last resort -- which is also how shipping titles do it, since IRX modules live on the disc. **Debugging technique that settled it in one step: build ps2sdk's own playadpcm sample and run it through the same harness.** A known-good reference separates "my code" from "this environment" faster than any amount of staring. |
| 2026-08-01 | **audsrv CANNOT stop a sounding ADPCM channel -- so it cannot preempt** | Its ADPCM surface is play / set-volume-and-pan / is-playing / free, with no stop. audsrv_ch_play_adpcm on an occupied channel returns -AUDSRV_ERR_NO_MORE_CHANNELS. Voice STEALING is therefore impossible: when all 24 voices sound, a new sound is dropped no matter its priority, and stop() can only retire the handle while the sample plays out. This is exactly the condition plan section 9 anticipates ("write a custom .irx only if the mixing model proves insufficient") -- now measured rather than assumed. The steal POLICY is implemented and tested (steal_candidate_voice names the victim) so the custom IRX has a specification to satisfy, and audio::can_preempt_voices() is the switch that flips when it exists. |
| 2026-08-01 | The host build models the SAME constraint deliberately | A host build that allowed preemption would let unit tests assert behaviour the console cannot deliver -- worse than not testing it. The host differs in exactly one documented way: with no SPU2 there is no notion of a sample ending, so stop() frees the slot immediately. |
| 2026-08-01 | **Never block a frame on audsrv_wait_audio** | It sleeps until the ring has room. Feeding through it stalls the game loop for whole frames at a time. The feeder asks audsrv_available() and hands over exactly what fits, which is also self-regulating: the sustained rate IS the hardware's consumption rate. |
| 2026-08-01 | audsrv's ring is counted in mono-equivalent units | A 22050 Hz **stereo** 16-bit stream implies 88,200 B/s, but the measured sustained feed is 45,433 B/s -- half, within 3%. The stream is continuous and underrun-free, so the practical answer is to assert continuity and log the rate rather than assert a constant derived from an assumption about audsrv's bookkeeping. |
| 2026-08-01 | An iteration count is not a duration | The first music test fed 240 chunks in 27 ms and "passed" its byte target by luck of thresholds -- the loop spins far faster than the SPU2 drains. Streaming tests must be paced by wall clock. |
| 2026-08-01 | The load-bearing audio assertion is that a voice RETIRES | Allocation bookkeeping can look perfect while the SPU2 plays silence. A 0.25 s clip whose voice frees itself ~500 ms later is only possible if the hardware really consumed the sample. |
| 2026-08-01 | ADPCM encoder quality (exporter self-check) | Exhaustive search over 5 filters x 13 shifts per 28-sample block, scored by squared error against the decoder's own arithmetic. Measured SNR: blip 51.4 dB, tone 57.7 dB, sweep 40.1 dB (the sweep is hardest -- the waveform never settles). The exporter decodes its own output and FAILS THE EXPORT below 20 dB, because a badly encoded sample has no runtime validator: the SPU2 plays it regardless. Reference to diff against: ps2sdk's adpenc, whose .adp header this format matches ("APCM", version, channels, pitch = freq*4096/48000, sample count). |

## Input, memory card, streaming (M10 tasks 2-4)

| Date | Finding | Detail |
|---|---|---|
| 2026-08-02 | **padInit returns 1 on success, not 0** | Unlike most of ps2sdk. Checking `!= 0` rejects a perfectly good pad stack. |
| 2026-08-02 | **libmc paths are RELATIVE TO THE CARD** | mcOpen/mcMkDir take the port and slot as arguments, so a "mc0:" prefix makes the driver look for a directory literally named `mc0:`. Writes then "succeed" into nowhere. Paths are `/DIR/FILE`. |
| 2026-08-02 | **mcOpen takes FIO flags, NOT newlib's fcntl.h ones** | fio's read-only is 1; newlib's O_RDONLY is 0. Including `<fcntl.h>` therefore opens every file in mode 0 -- not a mode at all -- and the card answers with a permission error that reads as "card is write protected". The values are in ps2sdk common/include/io_common.h and the module now spells them out locally. |
| 2026-08-02 | **A memory card will not short-read** | Asking mcRead for more bytes than the file holds fails the whole call rather than returning what is there. Seek to the end for the size, seek back, then read exactly that. |
| 2026-08-02 | The real sceMcRes codes are worth reading, not guessing | -2 is NO FORMAT, -3 is FULL DEVICE, -4 is NO ENTRY, -5 is DENIED PERMIT (libmc-common.h). An invented mapping turned a permissions failure into "card is full" and sent the debugging in the wrong direction. |
| 2026-08-02 | PCSX2's virtual cards start UNFORMATTED | Which is a genuine state a console can present, so the runtime handles it: `memcard::format()` exists, documented as destructive and gated behind player confirmation in a real game. Verified end to end -- format, save, load round trip (`level=7 volume=0.75 player='ASH'`), icon.sys present. |
| 2026-08-02 | A default enum value of "pending" made a queue look permanently full | `RequestState::Pending = 0` meant every freshly initialised slot read as an outstanding request, so `stream::request()` refused everything. Zero must mean IDLE. Caught immediately by the tests, which is the argument for writing them alongside the module rather than after it. |
| 2026-08-02 | Pads on PCSX2: bring-up is testable, presses are not | Port 0 reaches STABLE and polls real (slightly drifting) stick values -- 120/123 rather than a clean 128 -- which is why a deadzone exists. Nothing presses buttons, so mapping, edges, deadzone and pressure are covered by `inject_frame` in the host tests instead; that entry point doubles as the replay hook for recorded input. |

## Scene loading (M10 task 5)

Acceptance run: `samples/20-scene-stream`, which loads a 430 KB scene
additively while streamed music plays and asserts both.

| Date | Finding | Detail |
|---|---|---|
| 2026-08-02 | **The parse, not the read, is what stalls an async load** | Measured on the acceptance run: read 5 ms across 7 frames, parse **149 ms in one frame**. Async loading as first built made the READ incremental and left the parse as a single blocking step, which is the half that actually hurts. The 149 ms drained audsrv's ring and produced one audible dropout -- caught only because the sample asserts `music_underruns() == 0` while a load is in flight. |
| 2026-08-02 | **That 149 ms was `crc32`, not parsing** | `P2bFile::parse` checksums every section, and the checksum was the textbook bit-at-a-time loop: eight shift-and-mask steps per byte, ~3.4M inner iterations for a 430 KB scene. Switching to a 16-entry nibble table (64 bytes of .rodata, values identical by construction) took the parse to **64 ms** and the dropouts to zero. A 256-entry table would be faster still; 64 bytes was chosen because the EE's data cache is 8 KB and a bulk scan is already thrashing it. |
| 2026-08-02 | **`World::load` reset the geometry counters but not the animation ones** | `m_skinned_count`, `m_skeleton_count`, `m_clip_count`, `m_controller_count` and `m_skinned_mesh_count` were left as they were. A World is not always fresh: a non-additive scene change loads into the running one, and `append()` parses into a reused scratch world. So every scene reload accumulated the previous scene's characters. It survived M9 because it needs THREE loads to show up -- the second inherits the first's tables, the third overflows them -- and no test or sample had ever loaded three scenes in one run. Surfaced as `additive scene does not fit`. |
| 2026-08-02 | An overflow error that names no table costs an afternoon | The same failure originally read `additive scene does not fit`, with nine candidate tables. `append()` now names the one that filled up, which turned the diagnosis above from guesswork into one run. |
| 2026-08-02 | Each additive load needs its OWN buffer | Meshes point straight into the container they were read from (zero copy), so reusing one scene buffer for a second additive load pulls the geometry out from under the scene already running. The sample keeps three. |
| 2026-08-02 | PCSX2 cannot gate the 8 s scene-load budget | `host:` reads have no seek cost at all -- 430 KB arrived in 5 ms. The acceptance number (71 ms end to end) proves the interleaving and the parse cost, and says nothing about CDVD. Real timing is a hardware item. |

## Disc layout planning (M10 task 4)

| Date | Finding | Detail |
|---|---|---|
| 2026-08-02 | The planner has to read PCSX2's log, timestamps and all | `tools/disc/layout_planner.py` scrapes `M10_TRACE` lines from the emulator log, which carries a `[    1.6175] ` prefix on every line. An anchored regex silently found nothing and reported an empty trace. |
| 2026-08-02 | A raw-dump fallback must not accept prose | With no `M10_TRACE` lines present the tool falls back to "one path per line", which happily turned `[m10] nothing happened` into a file name. Lines now have to look like a disc path (no spaces, an extension) to count. |
| 2026-08-02 | Verified end to end | Planner -> `mkps2iso disc.xml` -> a 4,063,232-byte ISO with the six files packed in first-access order. Reported seek cost on that trace: 1021 sectors planned vs 1392 unplanned. |

## Physics (M11)

Acceptance run: `samples/21-kart` -- 2,074 terrain/obstacle triangles, a
driving kart, a trigger volume and a 200 m/s tunnelling case.

| Date | Finding | Detail |
|---|---|---|
| 2026-08-02 | **A character standing on the ground could never walk** | Its capsule touches the floor exactly, "touching" counts as overlapping to `capsule_triangle`, so every sweep reported an immediate zero-fraction hit and the controller was pinned by the floor it was standing on. Two fixes, both needed: the sweep capsule is a skin width thinner than the nominal shape (which is what `skin_width` is FOR), and an initial overlap has to exceed a tolerance rather than being a mere touch. Three character tests failed identically -- all showing a position that never changed at all, which is the signature of a blocked-at-t=0 sweep rather than a maths error. |
| 2026-08-02 | **Nothing ever slept, because the sleep check ran one phase too early** | Sampled right after integration, a body resting on the floor always looks like it is moving at 0.33 m/s: gravity has just been added and the contact impulse has not yet taken it away. So the timer reset every step and no body ever settled. Moved to after contact resolution, which is the honest measure of "is this still moving". Also removed the sleep-timer reset from static contact resolution, and made dynamic pairs wake each other only on a MEANINGFUL approach -- a resting pile is in contact every step, and waking on that keeps it awake forever. |
| 2026-08-02 | `validate_bvh` reports success by writing an EMPTY string, not a null one | `load_physics` assigned straight into its own error pointer and then treated non-null as failure, so every valid collision file was rejected with a blank message. A blank error message is itself the clue: the code that set it had nothing to say. |
| 2026-08-02 | Measured physics cost on the EE | **71 us average, 154 us worst** per fixed step over 2,074 triangles with a kart driving and a trigger active, against the plan's 4 ms budget (15.3) -- 56x under. 7,144 BVH nodes and 1,740 triangle tests over 300 steps, so the tree is doing its job: a linear scan would have been 622,200 triangle tests. |
| 2026-08-02 | Anti-tunnelling verified at 6.67 metres per step | 200 m/s at a 1/30 s step, against a wall far thinner than that. Sweeps advance at half the capsule radius and then bisect eight times, so nothing thinner than the shape can be stepped over. A discrete overlap test at the end position would have found the kart cleanly past the wall and reported nothing. |
| 2026-08-02 | Transient penetration during a fast landing is real and expected | The kart dropped from 6 m reached -1.31 at its deepest before being pushed back out. One position-correction pass at 0.8 removes most of an overlap per step, not all of it, so a fast impact takes a few steps to resolve. Resting penetration on a flat floor is well under 0.1 (asserted by `ABodyComesToRestOnTheFloorAndSleeps`). This is the documented cost of the 4 ms budget, not a bug -- but it is why a thin fast-moving object should use the swept controller rather than a rigid body. |
| 2026-08-02 | The golden checker is not safe to run in a back-to-back loop | Running all five goldens in one shell loop reported 02-scene-graph as changed; run on its own it passes with all 512 tiles bit-identical. The samples share `/tmp/ps2-emu-stage/emulog.txt`, so a fast successive run can read the previous sample's log. Check goldens one at a time, or give each an `EMU_TEST_STAGE` of its own. |

## Editor build pipeline (M12)

Verified by running the whole pipeline headlessly against the user project:
`Unity -batchmode -quit -executeMethod Ps2.Editor.PS2BuildBootstrap.BootstrapAndBuild`.

| Date | Finding | Detail |
|---|---|---|
| 2026-08-02 | **Unity DELETES Temp/ when the Editor exits** | Plan 13.3 puts intermediates in `Temp/PS2Build/`, but in batch mode -- which is how CI builds -- the Editor exits after every build and takes the whole directory with it. The incremental cache therefore never survived a single run and "incremental" silently meant "always clean". Moved to `Library/PS2Build/`, which Unity preserves and already gitignores. |
| 2026-08-02 | **The AOT BCL has no netstandard.dll; it is in `Facades/`** | `unityaot-win32/` itself does not contain it, so any assembly targeting netstandard2.x -- which the shim does -- fails in UnityLinker AND il2cpp with "netstandard was not resolved up front", an error naming a facade nobody referenced on purpose. Staging `unityaot-win32/Facades/*.dll` alongside the BCL fixes both. |
| 2026-08-02 | **The game assembly must be RECOMPILED against the shim, not copied** | Unity's own `Assembly-CSharp.dll` references `UnityEngine.CoreModule` and friends, none of which exist on a PS2, so handing it to UnityLinker fails with "Failed to resolve assembly: UnityEngine.CoreModule". The shim provides the same type names in the same namespaces, so the SOURCE compiles against either -- only the compiled references differ. Plan 13.3 step 5 says "Roslyn recompile against the shim" for exactly this reason; copying is a shortcut that cannot work. |
| 2026-08-02 | Unity's bundled `csc.exe` cannot be launched directly | It fails with "Could not load file or assembly 'System.Text.Encoding.CodePages'": it expects to be started by Unity's own build host. `dotnet exec <sdk>/Roslyn/bincore/csc.dll` carries its own resolution and is what build_m6/build_m7 have used all along. |
| 2026-08-02 | Compiling against the shim needs an explicit `-r:netstandard.dll` | The shim targets netstandard2.1, so its public signatures name types from the facade; without the reference every use fails with "The type 'Object' is defined in an assembly that is not referenced". |
| 2026-08-02 | `UnityLinker --rule-set` rejects anything but its own enum names | 'balanced' produces "Requested value 'balanced' was not found" and a fatal error. Only `conservative` and `aggressive` are used, because those are the two this project has actually run. |
| 2026-08-02 | il2cpp-port's CMake had not kept up with M10 | Linking the game ELF failed on `padInit`/`padPortOpen`: the runtime gained libpad, libmc and audsrv in M10, so everything linking `libps2ur.a` needs `pad mc audsrv` too. Found only when the pipeline first tried to link a real game, 65 seconds into a build. |
| 2026-08-02 | Measured build times | Clean build 19.6 s; incremental (scripts and scenes unchanged) **8.5 s**, with export, strip and IL2CPP all skipped by content hash. Output: 11,133 KB ELF, 11,680 KB ISO with SYSTEM.CNF. Plan D6 asks for a script-only change in under 3 minutes; this is well inside it, though on a one-scene project. |
| 2026-08-02 | ~~KNOWN GAP: the linked ELF is still M6's sample host~~ | RESOLVED the same day: `il2cpp-port/main_game.cpp` is the generic game host. It loads the boot scene named by the generated `game_config.h`, binds the world, brings up physics/input/audio, creates the scene's Rigidbodies and scripts, and drives `Runtime.Tick` forever. |
| 2026-08-02 | **A 6 MB static scene arena starved il2cpp's metadata loader** | Symptom was stores through a near-null pointer right after `global-metadata.dat` opened. A 6 MB `.bss` arena plus an 11 MB development ELF leaves too little of 32 MB for the metadata and the 4 MB GC heap, and il2cpp's loader writes through the null it gets back instead of checking. Capped the boot arena at 2 MB; streaming and additive loads take their memory from the platform heap. |
| 2026-08-02 | **The build reported success while producing no ELF** | The native step called `ctx.Warn(...)` and returned when the game executable failed to compile, from when the game host was an experiment bolted onto a runtime-only build. A compile error therefore came back as `succeeded: true` with an empty `elfPath`. Both that step and Package now throw, and `Build()` ends with an explicit completeness check: if the output directory is missing the ELF or any content file, the build is reported as FAILED even though every step "passed". Success is now defined as "the output is loadable", not "nothing threw". |
| 2026-08-02 | **The incremental cache did not include the build code itself** | Fixing the scene exporter changes no scene, no asset and no profile, so `IsUpToDate` said yes and the build reused the `.p2b` the *broken* exporter had written. The fix appeared not to work and cost a full debugging pass on an already-fixed bug. Every cacheable step now mixes in `PS2ContentHash.OfBuildCode(packageRoot)`, a hash of the package's own `.cs` files. The cache was answering "have the inputs changed?" when the question is "would this step produce the same output?". |
| 2026-08-02 | **Three parts of the BVH module disagreed on how to spell an empty tree** | `build_bvh` emitted one node with `count = 0` and inverted bounds; `validate_bvh` reads `count == 0` as "internal node, children at first and first+1" and rejected it as pointing outside the array; and it also rejected zero nodes outright ("a tree with no nodes is not the same as an empty tree"). The C# baker independently made the same choice as `build_bvh`, from the same spec. Nothing caught it because nothing had ever built a BVH over zero triangles -- which is every scene whose colliders are all primitives. **An empty tree is now ZERO nodes** in all three. The existing test `AnEmptyMeshBuildsAnEmptyTreeRatherThanFailing` asserted the broken behaviour, so the test pinned the bug: the fourth time in this project that a passing test has pinned determinism rather than correctness. |
| 2026-08-02 | **The scene exporter never called the collider exporter** | `P2bPhysicsExporter` was written, tested and complete at M11, and `P2bSceneExporter` never invoked it -- so no build ever contained a PHYS section. Likewise `Rigidbody` had a shim class, a native body table, a solver and a bridge, and no record of it in SCEN, so `GetComponent<Rigidbody>()` returned null on target and the user's script threw a `NullReferenceException` on the frame they pressed a button. Every individual piece was present and tested. What was missing was the call between them, which is precisely what a per-component unit test cannot see. |
| 2026-08-02 | Physics on target, end to end | `samples`-free acceptance: a Unity scene with a Cube (BoxCollider + Rigidbody, mass 1, gravity on) and a ground box, exported and booted in PCSX2. `[game] collision: 2 colliders, 0 triangles, 0 nodes`, `[game] 1 rigidbodies`, then from managed code `GetComponent<Rigidbody>()` non-null with the Editor's mass/gravity/kinematic values, `AddForce` applied, and after 30 frames `dy=3.69 dist=4.57 vy=-2.69` read back through `ps2ur_phys_body_get_velocity` -- a body that is genuinely bound to the native table and being integrated. |

## Still open

| Item (plan ref) | Status |
|---|---|
| bdwgc `gcconfig.h` PS2 constants (`il2cpp-port/bdwgc/`) | RESOLVED at M6: stanza shipped in `il2cpp-port/patches/` per plan 11.4 (`ALIGNMENT 4`, `CPP_WORDSZ 32`, `DATASTART/DATAEND` from `_fdata`/`_end`, `STACKBOTTOM` captured in `main()`, GET_MEM over memalign, threads off). See the IRIX5 trap above; `gctest` on-EE remains a nice-to-have (il2cpp's own allocation suite exercised it instead). |
| Offline builds | OPEN: `runtime/tests` fetches GoogleTest v1.14.0 over the network at configure time, and `install.ps1` fetches MSYS2 packages. Neither has a vendored/mirrored fallback. |
| `mkps2iso` | RESOLVED at M10: installed locally at `tools/mkps2iso/` (gitignored -- it is a third-party binary, not ours to vendor) and proven end to end against `tools/disc/layout_planner.py`'s generated script. Still needed properly for M12 (ISO packaging); `doctor.sh` reports its absence as optional. |
| Docker image + CI (M0 tasks 1, 7) | OPEN: Docker is not available on this machine and there is no CI runner. `tools/ci/README.md` tracks it. The plan's note that emulator tests need an out-of-band BIOS still stands -- never download one. |
| Host SDL2 rasteriser stand-in (M1 task 4) | OPEN: deferred until M2 defines the `gfx::Device` interface it must stand in for. The rest of the host build (platform layer, allocators, math, tests) is done. |
| Section 7.2 | OPEN: section 7 jumps from 7.1 to 7.3 in `ps2port.txt`; the "not supported" list appears to be missing. Sections 9-18 are now present. |

## Imported characters (M12.5, 2026-08-02)

Found by running the user's own scene, not by a test. Every one of these was
green at the unit level.

1. **An imported character exported as nothing at all.** `P2bRigExporter.Bake`
   kept the FIRST `SkinnedMeshRenderer` it found and returned `null` if that
   one was unusable. Unity-chan's first in traversal order is `BLW_DEF`, a
   bone-less eyebrow plane, so the whole character was dropped and the `.p2b`
   had no `SKEL`/`ANIM`/`CTRL`/`SKMS` section at all. The scene loaded, the
   ground drew, and nothing said anything was missing. Diagnosis came from
   dumping the built container's section table -- start there, not in the
   exporter.

2. **A character is many renderers over one skeleton.** 19 for Unity-chan,
   one per material; 15 of them skinned. The exporter now unions their bones
   into one skeleton and emits one `SKMS` each. Bone-less renderers are
   skipped with a warning naming the fix rather than aborting the scene.

3. **Sharing an animator cannot be inferred from skeleton + controller.**
   One character's 19 renderers must share one `anim::Animator` (they show one
   pose); three characters built from one rig must NOT (M9's sample drives
   `world.animator(1)` through a crossfade while animator 0 holds the golden
   pose). Both cases have the same skeleton and controller. The exporter now
   groups by the `Animator` component driving each renderer and writes the
   group into the `SkinnedMeshRenderer` payload -- the field that used to hold
   an ignored skeleton index. **Any `.p2b` written before this with more than
   one character reads as one animator and must be re-exported.**

4. **A shared animator must be advanced once, not once per renderer.**
   `update_animators` walked renderers. With sharing that ticks one animator
   19 times a frame: not a slow clock, a 19x fast one.

5. **The real limits were nowhere near a real character.** 64 bones (needed
   140, union across renderers), 8 clips (24), 16 states (24), 256 tracks
   (140 bones x 3). Raised to 192/32/32/640. `kMaxPaletteBones` = 24 is
   untouched: that one is the VU1 data-memory layout, i.e. actual hardware.

6. **`scene::World` went to 1.17 MB and 16 tests segfaulted at once.** They
   allocated it on the stack. `test_scene_load.cpp` already used `static` --
   someone had hit this before and fixed it locally instead of preventing it.
   Now `static_assert(sizeof(World) < 2 MB)` and every site is `static`.

7. **The boot-scene arena was a hardcoded 2 MB while the profile's
   `assetPoolMb` governed nothing.** A scene with one character in it is
   5.5 MB, so this failed on the first real scene rather than at the margin.
   The M12 reasoning behind the 2 MB (a big `.bss` array is committed before
   il2cpp allocates, and its metadata loader writes through the null it gets
   back) was right about `.bss` and wrong about the size: it is now a heap
   allocation of `PS2_GAME_ASSET_POOL_BYTES`, taken before `bridge::init()`,
   so nothing unused is committed and the profile setting finally means
   something.

8. **Texture export required Read/Write Enabled**, which almost no imported
   art has, and failed on block-compressed formats besides. Non-readable
   textures now go through a RenderTexture blit. Two parts of that are decided
   by the graphics API rather than by our code -- whether `Blit` flips
   vertically, and whether the sRGB round trip is faithful -- so
   `VerifyBlitRoundTrip` asserts both once per domain reload against a probe
   that is asymmetric in both axes. It also catches `-nographics`, where the
   blit silently produces nothing.

## Rigid-bound models (M12.5, 2026-08-03)

A model can be Humanoid with a valid avatar and still contain zero
SkinnedMeshRenderers: if its meshes carry no skin weights they import as
MeshRenderers parented to bones, and Unity animates them by moving the bone
TRANSFORMS. It looks identical in Play mode, which is why "the rig tab says
Humanoid" convinces everyone the model is skinned. The tell in a build is
the .p2b: a skinned character contributes SKMS sections and no MESH; a
rigid-bound one contributes only MESH (33 of them here) and bakes no rig.

Support: when Bake() finds no usable SkinnedMeshRenderer but an Animator
with a controller exists, the skeleton is the Animator's strict-descendant
transform hierarchy (identity bindposes -- nothing is skinned, the pose IS
the transform), clips sample exactly as before, and the SCEN type-7
component is the only carrier. At load, an Animator component with no
skinned renderer binds its own pool slot flagged drives_entities: each
skeleton bone is matched BY NAME HASH to an entity under the Animator
(both sides already wrote (uint)Fnv1a64(name)), and update_animators writes
the sampled pose into those entities' locals -- the dirty-tracked matrix
pass then moves exactly the subtree that changed. Root motion stays off.

Consequences worth knowing: bones bind by name, so renaming a bone after
export breaks its binding (the exporter warns, and warns again on duplicate
names); and an Animator whose rig failed to bake is now named at export
instead of surfacing as GetComponent<Animator>() == null on target.

Addendum, same day: the first run animated ONLY the hair. A humanoid clip
stores the body in MUSCLE curves that exist only through avatar retargeting;
`AnimationClip.SampleAnimation` evaluates transform curves directly (the
exporter even forced clip.legacy=true to use it), so the body froze at rest
while the generic hair/ribbon bones in the same clip moved -- half a
character, from two storage forms in one asset. Humanoid clips now sample
through a PlayableGraph driven by the Animator (foot IK off); generic clips
keep the proven SampleAnimation path. This was the first humanoid content
ever through the sampler -- M9's parity golden was generic clips.

## Golden captures depended on the user's GUI settings (M12.5, 2026-08-03)

All five goldens "regressed" with no rendering change anywhere near them.
run-emu-test.sh inherited the user's global PCSX2.ini, whose Renderer = -1
(automatic) had picked a different GS backend than when the goldens were
captured -- the user had been in the PCSX2 GUI all day -- and tile CRCs are
a function of the rasterizer. The golden had pinned the RENDERER along with
the image.

The harness now regenerates a PORTABLE config (pcsx2/inis) from the user's
ini on every run: Renderer forced to 13 (software -- the only bit-exact,
GPU- and driver-independent basis for a CRC golden), EnableEEConsole on,
and every relative folder path absolutized to the user's config root,
because portable mode moves the root and the BIOS would silently vanish.
All five goldens re-baselined under the pinned renderer, deliberately.

Residual: 02-scene-graph produced ONE spurious mismatch immediately after
its re-baseline and passed twice after. Intermittent, so not chased to
ground; the suspect is IOP module load timing shifting the capture frame.
If it recurs, that is where to look.

Addendum (PlayerPrefs boot): "loadmodule: id -203" for mcman/mcserv on a
build whose every step reported success. The Package step's IOP module list
said "freemcman.irx"/"freemcserv.irx" -- names this sdk does not ship -- and
its copy loop skipped missing files silently, while the runtime asks for
"mcman.irx"/"mcserv.irx", which the sdk DOES ship and nothing copied. Names
fixed, and a listed-but-missing module is now a build warning naming the
boot symptom. The embedded-blob fallback in memcard.cpp is exactly what the
list's own comment says cannot be trusted (SifExecModuleBuffer, M10).

## PS2ParticleSystem (M12.5 task 4, 2026-08-03)

ADR-011: CPU sim (8 systems x 128 particles, xorshift32 per system so a
reload replays identically), billboarded on the EE from the view matrix's
rows, drawn through vu_unlit_tex with the transparent-pass GS state
(alpha 0x44 / additive 0x48, Z test on, Z write off). No new microprogram:
a few hundred quads is under a millisecond of EE time, and M4 is the
receipt for what a new .vsm costs. Batches stage 13 quads (78 verts x 3
qwords = 234, under the 255-qword VIF NUM ceiling) in a static scratch
reused after each per-system kick+wait. Bursts are QUEUED at load and fired
on the first update -- spawning at parse time would read world matrices
that do not exist yet.

## uGUI subset (M12.5 task 5, 2026-08-03)

Retained draw table in the World (component 11), rects BAKED at export by
real RectTransform anchor math against the framebuffer, drawn by the
renderer in a SECOND frame packet after every 3D kick -- the M8 debug
overlay rides the CLEAR packet and 3D draws over it, fine for stats, wrong
for a menu. Interaction is entirely managed: PS2UINavigation moves D-pad
focus in hierarchy order, Cross submits, Left/Right step sliders; the
Slider writes its fill element's rect through the same bridge setter
scripts use. Deviations 30-31.

And the "intermittent golden flake" is DEAD, with a diagnosis that changes
its meaning: check.sh told the emulator runner to wait for "GOLDEN_TILE",
so the runner declared success on the FIRST tile line and killed the
emulator mid-emission -- the capture was a PREFIX of the image (64 or 116
of 02-scene-graph's 512 tiles, depending on flush timing). Single-burst
samples usually survived the race; 02's eight poses spread over 300 frames
usually did not. The runner now waits for the sample's COMPLETION token
(PS2UR_TOKEN_*), and 02 passes 512/512 three runs straight. A retry-once
guard remains in check.sh and now exists to catch the next NEW flake, with
its message saying a real regression fails twice.

## The first real Canvas found two bugs the layers could not (M12.5 task 5, 2026-08-03)

The user added a Canvas with one Legacy Text and the scene refused to
boot: "ui element payload truncated". The parser demanded 88 bytes for
the UIElement payload; the format -- and the exporter -- say 84 (36 fixed
+ 48 text). The 4-byte overrun only fires when a UI element's payload is
the LAST thing in the SCEN section, because payloads are addressed by
explicit offset and the reads themselves only span 84 -- so every layout
where anything followed the element passed. A Canvas hierarchy at the
bottom of a scene produces exactly the fatal layout, and no synthetic
scene existed to produce it first: the UI component shipped without the
byte-level round-trip test the rigidbody component got, the discipline
whose entire point is that a size drift "fails a test instead of failing
in PCSX2". test_scene_ui.cpp now writes the payload from the format doc
with the last element flush against the section end (7 tests, 285 total).

Fixing that exposed the SECOND bug behind it, and it is the M12 defect
class in its purest form yet: main_game looked up CreateUIGraphic /
CreateUIButton / CreateUISlider, null-checked them, failed the boot if
they were MISSING -- and never called them. The renderer draws the canvas
straight from the World table, so a menu would LOOK wired while every
Button was unreachable and PS2UINavigation had zero selectables. Present,
tested, null-checked, and never invoked; only the on-target acceptance
run could see it. The create loop now exists (graphics first, then
selectables in table order) and prints "[game] ui: N elements, N buttons,
N sliders" as the cross-layer proof.

Verified on the user's own build output: SampleScene.p2b (182 entities,
Canvas + Text + particles + audio + animator + PoseChanger) boots to
PS2UR_TOKEN_GAME_OK with "ui: 1 elements"; goldens 512/64/64 unchanged.

Incidental, found because grep started treating p2b_scene.cpp as a
binary file: three '\0' char literals had been written as LITERAL NUL
BYTES in the source (the printf/heredoc mangling class, third sighting).
GCC compiles a quote-NUL-quote literal to the correct value, so behaviour
was right and every build was green -- but the file was invisible to
text search, which is how the class of tool that would find the next bug
gets blinded. ASCII-only means checking for it; the fix was a byte-level
replace.

## Texture alpha never existed, and the overlays hid it for six milestones (M12.5 task 5, 2026-08-03)

The user's first Canvas screenshot showed text glyphs in solid black
cells and UI sprites with their dead corners filled in. Root cause:
set_texture and set_texture_indexed have passed TCC=0 since M2, which
tells the GS to take fragment alpha from the VERTEX (always 0x80) and
ignore the texture's alpha entirely. The font's alpha test therefore
discarded nothing, and sprite transparency could not exist. Nothing ever
caught it because the two consumers of texture alpha until now were the
debug overlays -- white text on dark translucent panels, where black
glyph cells are nearly invisible -- and because the golden readback
captures the framebuffer before the overlay pass draws. TCC is now 1 in
both binds. All five goldens pass UNCHANGED: the exporter has always
baked CLUT alpha in the PS2 0..0x80 range with opaque = 0x80, so
MODULATE's At x Af is the identity on every opaque texture; the change
lands only where alpha is real.

Same session, same screenshot: 9-slice. Unity's rounded UI sprites are
authored as Sliced and look smeared under a whole-texture stretch. The
sprite's border rides the image record's unused text bytes (four f32,
L T R B -- sliders already overload those bytes, so an image that is
ALSO a slider background keeps value/maxW and loses its border), and
the overlay draws up to nine patches in one packed GIF with clamp on.
Borders map 1:1 to framebuffer pixels like every other canvas
measurement (deviation 30). A byte-order test pins the packing against
the renderer's memcpy order. Scenes exported before this change carry
zero borders and simply keep the old stretch until re-exported.

Also from the same screenshot: the bind callback now reports texture
dimensions and can refuse. textured_rect had been told every texture
was 256x256 -- a 32 px sprite tiled eight times across its rect (the
"row of blobs") -- and a bind of a non-resident index silently drew
with whatever texture was bound last. And the font atlas upload inside
init_ui ran with no frame open, so the upload died in a stale packet
and every glyph sampled uninitialised VRAM; init_ui now frames its own
upload. Each of these four defects was invisible until a REAL canvas
with REAL art hit the screen -- the acceptance run keeps out-earning
the layer tests.

## The pad config flood was a handshake livelock (M12.5, 2026-08-03)

PCSX2 logged "Pad: DS2 Config Finished" five times a second for 24
seconds of every boot. Ours, not the emulator's: every pad command is
asynchronous, and input::update() re-issued padSetMainMode on each retry
and then immediately asked for the current mode -- so the answer always
described a request still in flight, the "did it take" check could never
pass, and all 120 retries ran their course as back-to-back config
sequences. Worse than the noise: padEnterPressMode was being issued in
the same breath as the busy main-mode request, so pressure mode NEVER
actually engaged -- the pad reported RB: D (digital response bytes only)
forever while pad.pressure claimed otherwise.

Now a staged handshake: one request in flight, verified only after
padGetReqState reports it complete; failed requests re-issue, a
240-frame budget gives up quietly on pads that cannot comply, and a
disconnect resets the stage so a re-plugged pad renegotiates. A boot now
logs exactly five config lines -- boot default, analog locked, press
mode entry, response bytes switching to D+A+P (0x0003FFFF, pressure
genuinely active for the first time), actuator alignment -- and then
nothing. Real-hardware caveat stands per plan 14.5: PCSX2's pad model
is forgiving about timing, so the staging discipline matters MORE on a
real DualShock, not less.

## Text.alignment (M12.5 task 5, 2026-08-03)

The button label drew at its rect's top-left; Unity had it MiddleCenter.
The anchor now rides bits 8-11 of the text_scale word (3x3 TextAnchor
grid = column mod 3, row div 3; old files carry zeros and keep the old
top-left), and the overlay aligns PER LINE at draw time -- measured from
the CURRENT string, because Text.text is mutable at runtime and a baked
offset would go stale on the first SetText. draw_text_aligned measures
with the same advance rules draw_text draws with; a drift between the
two would misplace text by exactly the mismatch.

## M12.5 task 5 ACCEPTED on target (2026-08-03)

Accepted by the user in their own scene rather than a repo sample: title
text, a button, a volume slider, D-pad focus, X firing onClick, real
9-slice on the default rounded sprites, texture alpha, and a
MiddleCenter button label actually centred. Boot line: "ui: 7 elements,
1 buttons, 1 sliders", GAME_OK, five pad-config lines then silence.
The milestone doc's token-asserted menu sample and its golden land with
Task 6's combined D1-candidate sample. Known gap, stated in deviation
30: Text wraps only on explicit newlines -- no automatic word wrap
against the rect (marginal under a 48-byte cap).

## SceneManager.LoadScene was finished and unreachable (M12.5, 2026-08-03)

The user asked for a title screen whose Start button loads the game
scene -- the first real use of SceneManager -- and the M10 loading stack
turned out to be complete, sample-verified, and unreachable from a game:
the host never called stream::init() (every load refused), never called
bridge::bind_scene_buffer() (no buffer to read into), and had no code to
rebuild ANYTHING derived from the world after a swap. Three missing
calls between finished layers, in one feature.

The host now owns scene transitions: a second asset-pool arena is
reserved at boot (transitions cost one extra pool; a game that cannot
fit both says so and runs without them), the bridge counts ACTIVATIONS
-- a blocking LoadScene runs read-to-activation inside one managed Tick,
so state polling can never see it; only a counter can -- and on a swap
the host tears down the managed world (Runtime.ResetForSceneLoad: every
OnDisable/OnDestroy, wrappers, navigation, collider maps), clears bodies
and SPU2 clips (audio::reset_clips -- audsrv has no per-clip free, so
its ADPCM arena is re-initialised), rebuilds static collision, frees and
re-uploads VRAM textures, re-uploads clips, and re-runs the same
instantiate pass boot uses, extracted into one function so boot and swap
CANNOT drift. Additive loads create only appended entries (table counts
snapshotted at begin) but occupy the second arena for good; deviation 32
states the bounds.

The old scene's managed objects run one last Tick against the already-
swapped world before the host rebuilds -- safe by construction, because
every cross-boundary handle is generation-checked (M7's design decision
paying off six milestones later).

NOT yet verified on target: needs a two-scene build, and the Unity
Editor holds the project lock. The title screen scripts + a scene
generator (PS2 > Create Title Screen) are in the user project; repo
side is proven by 287 host tests and clean builds of all three
toolchains.

## The first real transition found the parse-size mismatch (M12.5, 2026-08-03)

The user's title screen worked first try -- built, booted, navigated,
START fired, the loader read SampleScene.p2b and activated -- and the
host's post-swap re-parse refused the container: P2bFile::parse demands
that the header's total_size EQUAL the size argument, and the host
passed the arena capacity. The loader's bytes_read() now rides the
bridge (scene_last_load_bytes) and the host parses with the exact
count. The strict equality stays: it is the check that catches a
truncated read, and the fix is to tell it the truth, not to loosen it.

## The focus highlight was white-on-white (M12.5, 2026-08-03)

The navigation focus tint has existed since task 5 -- and lerped the
target toward WHITE, on a default button sprite that is already white.
An invisible highlight, shipped working. Two fixes: the tint now goes
toward a warm gold (the PS2-era menu convention, deliberately stronger
than Unity's near-invisible ColorBlock default -- a D-pad menu on a TV
lives or dies by knowing where the cursor is), and behind it a latent
base-colour bug: the managed Graphic assumed white as its base, so
unfocusing a COLOURED button would have "restored" it to white. A new
bridge getter (ps2ur_ui_get_colour) seeds the managed colour from the
authored tint at create, so focus restores the truth.

## Baked Unity fonts (M12.5, 2026-08-03)

The user asked for "the font and sizing Unity uses", and the honest PS2
answer is to make UNITY do the rasterising: at export, every
(font, fontSize) pair a Text uses goes through
Font.RequestCharactersInTexture -- one request for all 95 printable
ASCII glyphs, because a second request can rebuild the dynamic atlas
and invalidate every CharacterInfo fetched before it -- and the glyphs
are read back out (per-pixel UV interpolation between CharacterInfo's
four corners, which quietly handles the rotated placements Unity's
atlas packer produces) into a shelf-packed atlas of white pixels with
coverage in alpha. That atlas rides the ORDINARY texture path: 256
alpha levels of one colour quantise losslessly into the 256-entry CLUT,
and TCC=1 (fixed earlier today) is what lets the coverage blend.
Metrics travel in a new FONT section (reserved by the plan since
section 8): 32-byte header, 12 bytes per glyph, advance in 12.4 fixed
point so sub-pixel spacing survives the container.

Runtime: World holds up to four gfx::UIFont tables; the text element
names one in bits 16-23 of the scale word (zero = builtin 8x8, which is
also the fallback when an atlas cannot bind -- degrade to READABLE, not
to invisible); draw_text_font measures and draws per line with one
shared advance rule. Orientation trap worth recording: the atlas is
written BOTTOM-UP like every Unity texture, because the texture
exporter flips rows on write -- store it top-down and every glyph
arrives mirrored. Font atlases are exempt from Texture Max Size
downscaling (every metric is a pixel coordinate into them), and
kMaxGpuTextures rose 8 -> 12 to hold them.

290 host tests (FONT round-trip, glyph-table truncation, missing-font
ref). On-target visual awaits the user's rebuild.

## The grey banded font, diagnosed from the container (M12.5, 2026-08-03)

The first baked-font screenshot showed dark, banded, ghosted text.
Rather than guess, the built TitleScreen.p2b was decoded offline
(sections -> CSM1 un-shuffle -> CLUT -> pixels): glyph SHAPES were in
the atlas at the right coordinates with sane proportional metrics --
placement, orientation and the draw path all correct -- but coverage
peaked at 34/128 (~27% alpha) across only EIGHT distinct levels. Two
export bugs, one readback: (1) the ReadPixels round-trip attenuated the
coverage and the code read only .a, when which channel a font atlas
carries coverage in is a backend detail -- now MAX over channels,
normalised so the peak is 100% (every real font has fully-opaque
cores); (2) the median-cut quantiser, built for art, collapsed the
alpha ramp to eight levels -- a font atlas's palette is known BY
CONSTRUCTION (256 levels of white), so it is now encoded directly:
index = coverage byte, CLUT = ramp, lossless with no quantiser in the
path. The offline decoder earns a note: five minutes of reading the
actual bytes replaced an afternoon of speculating about GS state.

## The invisible skinned character: a space the samples never tested (M12.5, 2026-08-03)

The user's imported PSX-style model exported cleanly (3 renderers, 64
bones, 24 clips) and drew NOTHING -- while the frame stats said
"skinned 3 in 56 batches, culled 0". Submitted every frame, zero
pixels. Offline simulation of the full vertex path (SKEL rest chain x
inverse binds x renderer entity world x camera, all decoded from the
built container) showed the vertices landing at world Y ~80 and BEHIND
the near plane: every triangle near-rejected inside VU1, which is the
one failure mode that renders as a clean empty frame.

Root cause: the runtime composes vertices as palette x renderer-entity
world, where the palette comes from the SKEL rest/clip chain. That is
Unity-equivalent ONLY if the rest chain's root space equals the bound
entity's space. The exporter wrote raw Unity LOCALS for rest and keys
and bound renderers to the SMR node entities -- and this FBX, like most
real imports, parks scaled non-bone nodes between the Animator and the
rig (a x100 unit-conversion node on the SMRs, a x12 armature). M9's
procedural rigs had identity everywhere, so the substitution was
silently exact for six milestones.

Fix, all export-side (no runtime change, no new ELF): rest transforms
and every clip key are exported relative to the bone's NEAREST BONE
ANCESTOR (composing through non-bone gaps) with the ANIMATOR as the
root reference; renderer records moved to the Animator's entity so the
runtime multiplies by the transform the data is relative to; culling
bounds mapped into the same space (a centimetre-authored mesh under a
x100 node has 0.01-unit mesh bounds -- wrong in either direction).
Root-bone clip keys matter as much as rests: hips tracks sampled as raw
locals are armature-relative, and playback through the folded chain
would re-apply the wrong space every frame.

Diagnosis chain worth keeping: exporter drop (Read/Write) -> container
decode (renderers present, entities x376) -> direct-boot variant ELF
(bypasses the title screen for a headless SampleScene boot: stats said
DRAWN) -> offline transform simulation (behind the near plane). Each
step replaced a guess with a number.

## The 8x body parts: stored bindposes vs the rig as it stands (M12.5, 2026-08-03)

After the space fix the character appeared -- textured, lit, real-font
UI beside it -- as correctly-placed bones wearing wrongly-scaled flesh:
"the body is above the head". Container decode again: the rest chain
carried the armature's x12.25, the STORED bindposes carried the SMR
node's x100, and their product put spine-skinned vertices 0.9 units
above the head bone. The mesh's stored bindposes describe the rig at
SKINNING time; import pipelines routinely rearrange unit-conversion
factors between armature and mesh nodes afterwards, and the two ends
stop agreeing. Unity survives because its bone worlds and its stored
binds drift TOGETHER through its own import fixups; an exporter that
mixes its own rest chain with the stored binds inherits the
disagreement raw.

Fix: bind poses are RECOMPUTED at export from the same transforms the
rest chain samples (bone.worldToLocalMatrix x smr.localToWorldMatrix),
so rest x bind cancels by construction and the runtime reproduces the
pose the Editor shows -- for any FBX, any import history. Requires the
scene pose to be the bind pose, which a character in its default
imported pose satisfies. Scale tracks: none are emitted for constant
bone scales (keyframe reduction), and the runtime falls back to rest
scale, which now carries the folded armature factor -- checked, sane.

## Three renderers, three mesh spaces, one bind table (M12.5, 2026-08-03)

The rig-diagnosis dump earned its keep on its first outing. It showed
(a) Unity's skinning formula and the exporter's producing IDENTICAL
vertices -- both correct, my earlier python simulation was the wrong
tool -- and (b) the actual defect in three numbers: the character's
renderers sit at different node transforms (arms y 1.54, body y -0.02,
head y 1.89), Unity's stored bindposes map from each renderer's OWN
mesh space, and the union skeleton keeps ONE bind per bone -- whichever
renderer contributed it first. The body's vertices went through the
arms' bind: displaced 1.56 units up, "the body is above the head",
literally. The BindposesAgree warning had been firing all along, with
advice that blamed the asset for what the union design could not
represent.

Fix: vertices are baked into REFERENCE (Animator) space at export and
binds are reference-relative -- renderer-independent by construction,
so the union can never conflict again. Side benefits: exported
vertices carry sane magnitudes (~1.9 units, not 0.01), and the culling
bounds need no separate space fix.

The runtime is exonerated by test_scaled_rig: the composed bone worlds
match Unity's MEASURED matrices from the diagnosis dump (spine at
(0, 1.00, -0.02), lossy scale 12.25) on the real container -- the
scale path M9's unit-scale rigs never exercised, now pinned against
ground truth rather than inspection.
