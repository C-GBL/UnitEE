# Hardware bring-up

Plan section 9, M13 task 4. This document is the procedure for taking a build
that works in PCSX2 and making it work on a retail console, plus the
catalogue of places the emulator is known to be more forgiving than the
silicon.

**Status: in progress.** A console arrived on 2026-09-08. The first boots
of the game ELF went black; the boot probe then established, rung by rung,
what this hardware does and does not do (the "First results" section below),
and the game's startup was corrected to match. On the second day the probe
booted from the hard disk through Open PS2 Loader and the game, with the
corrected startup, still did not: the "Second day" section below records
what that rules out and the two diagnostic builds that decide what is left.
Acceptance D3 (boots and plays on fat and slim hardware, from DVD-R and from
USB) remains open until the game itself runs.

## Why the emulator is not the last word

PCSX2 reimplements the machine at a level of abstraction that is generous in
exactly the places a homebrew engine is most likely to be wrong. The DMA
controller, the caches and the GS all behave more tolerantly than the chips
do. Risk R7 in the plan names this directly: "PCSX2 leniency hides hardware
bugs".

The mitigation the plan asks for is a leniency checklist maintained as the
engine is written, so that bring-up is a list of things to check rather than
an open-ended debugging session. That checklist is the next section.

## The leniency catalogue

Each entry is something this engine does that the emulator accepts, along
with what the hardware requires and how to verify it. Entries marked
**verified** have been confirmed correct in the emulator and are believed
correct on hardware for the stated reason; entries marked **unverified** are
the ones to check first when a console does something the emulator did not.

### DMA and cache coherency

| Item | Emulator | Hardware | State |
|---|---|---|---|
| Cache flush before a DMA read | Tolerates a missing flush in many cases, because it reads the same memory the EE wrote | The DMAC reads physical RAM and does not see dirty cache lines. Every buffer handed to a DMA channel must be flushed first | Handled: the chain flushes itself and every referenced block before kicking, a lesson the GIF path learned at M2. **Verified on hardware** for the GIF frame packet: the probe's blue rung |
| Chain buffer alignment | Accepts unaligned qwords | DMA tags must be qword aligned or the transfer runs off into whatever follows | Handled: every GS and DMA structure is 16-byte aligned with a static assertion on its size |
| Scratchpad addressing | Treats it as ordinary memory | Single-cycle memory at a fixed physical address, not covered by the cache and not visible to the conservative collector | Handled: the scratchpad is excluded from collector scanning by design. **Unverified on hardware** |
| Waiting for a channel | Returns promptly | A channel that is never waited on leaves the next kick writing into a transfer in flight | Handled: the chain waits before reusing its buffer |

### Graphics Synthesizer

| Item | Emulator | Hardware | State |
|---|---|---|---|
| Malformed GIF packets | Often ignored or drawn approximately | Can wedge the GS until reset | Unverified. The packet builders are the code to audit first if a console hangs with a black screen where the emulator drew a frame |
| Texture upload during drawing | Completes without visible cost | An upload on PATH3 stalls VU1 drawing on PATH1 | Handled by scheduling uploads at frame boundaries. The profiler's pipeline window is what would show a regression |
| VRAM overcommit | May silently succeed | Writes outside the 4 MB corrupt the framebuffer or the Z buffer | Handled: the allocator refuses and the texture is reported as skipped rather than uploaded on top of something else |
| CLUT format quirk (CSM1) | Matches | Requires the documented 8-entry swap within each 32-entry group | Handled and covered by a host test |

### Timing and boot

| Item | Emulator | Hardware | State |
|---|---|---|---|
| Boot ELF name versus disc serial | Boots anything | A non-American BIOS refuses a disc whose executable name does not match its serial | Handled: the build profile carries both and the validator checks they correspond. **Unverified on hardware** |
| Disc seek time | Effectively zero over the host filesystem | Roughly 100 ms per seek | Not measurable in the emulator at all. File order on the ISO is planned from a recorded access trace; the payoff can only be measured on hardware |
| IOP module load | Tolerant of load order | Requires SIF RPC and the IOP heap up before a module that allocates | Handled: the platform layer brings them up in order, a sequence found the hard way at M10 |
| Loading a module from an EE buffer | Works without any patch | The ROM loadfile has no such RPC; `sbv_patch_enable_lmb` must be applied first or the load never returns | **Found on hardware.** The game hung here under both launchers. Console builds apply the patch before any buffer load; PCSX2 tolerates it too |
| Launcher-resident IOP modules | Not a factor | uLaunchELF and Open PS2 Loader leave their own iomanX, fileXio and more resident; a boot that does not reset the IOP runs on those | **Found on hardware.** Console builds (host filesystem off in the profile) reset the IOP retail-style before loading anything. PCSX2 tolerates the reset, `host:` included; host-filesystem builds skip it because a ps2link loop on hardware would not survive it |
| VRAM readback (local-to-host transfer) | Copies VRAM out instantly | The reverse GIF path needs an exact BUSDIR, FIFO and channel-direction sequence and hangs on any mistake | **Found on hardware:** `read_framebuffer` hangs. It is a verification tool the game never calls; goldens stay emulator-only until it is fixed |
| Field timing | Vsync-locked cleanly | Interlaced field timing is stricter | The profiler reports dropped flips, which is the measurement that would catch this |

## Procedure

Everything below is the intended sequence. None of it has been executed.

### 1. Prepare media

- **DVD-R.** Build with ISO packaging enabled, burn at the slowest speed the
  drive offers. Older consoles are noticeably fussier about media than newer
  ones, and a bad burn presents as a boot failure rather than a read error.
- **USB.** Set the deploy target to a USB folder and copy the build to a
  FAT32 stick, then launch it with a homebrew launcher. This is the faster
  loop and the one to use for iteration.
- **Network.** With ps2client, the executable is pushed over the network and
  the console's output comes back on the same link, which is the closest
  thing to the emulator's log that hardware offers.

### First results from a console (2026-09-08)

The probe climbed red, orange, yellow and blue on a retail console launched
from uLaunchELF, and stopped on blue. That establishes, on silicon: the ELF
executes and its layout loads; the EE can write GS privileged registers; a
retail-style IOP reset comes back; iomanX and fileXio load from EE memory once
the buffer-load patch is applied; the GS initialises; and a frame drawn over
the GIF DMA path lands, cache flush and all. The rung that failed is the VRAM
readback, which the game does not use. The game ELF had gone black earlier
because its startup loaded IOP modules with neither the reset nor the patch,
ahead of any colour; both are now in the platform layer and the GS comes up
first.

### Second day (2026-09-09): the probe boots from the disk, the game does not

The probe, packaged as an ISO exactly like the game (its own serial,
SLUS-90002) and installed with HDL Installer, climbed to blue when launched
from Open PS2 Loader. The corrected game, as an ISO through the same path and
as a bare ELF through uLaunchELF, never showed a colour. Both launchers boot
one of our ELFs and not the other, so the difference is inside the game ELF.

What the emulator then ruled out, with the identical files:

- The game ISO boots in PCSX2 through the retail path (the kernel's
  LoadExecPS2 and the ROM loader on the IOP) to the title screen and a full
  profiler report.
- The game ELF launched by the real wLaunchELF inside PCSX2, using its
  auto-launch setting, boots to the expected missing-scene failure. The
  launcher's resident state is not the problem in the emulator.

What the two ELFs say about themselves:

- One load segment each, both at 0x00100000; the game's ends at 0x00A0DEC4
  with a 3.5 MB .bss. Same 128 KB stack at the top of RAM, same heap.
- No instruction the R5900 lacks in either. No double-precision FPU code,
  no ll/sc, no MIPS32 extensions.
- crt0, the kernel patches and the libc start-up have the same call graph in
  both, pthread-embedded initialisation included.
- The game runs 44 static constructors where the probe runs 3. The extra 41
  belong to libstdc++ and libil2cpp: they create pthread-embedded mutexes
  and a TLS key over kernel semaphores, register destructors, and allocate.
  main() then has a 40 KB frame and reaches GsDevice::init() with nothing
  in between, the probe's fourth rung.
- uLaunchELF's loader stub and Open PS2 Loader's EE core both live below
  0x00100000 and hand the ELF to the console's ROM loader (SifLoadElf and
  LoadExecPS2 respectively), so neither overlaps the game and the load path
  is the one every retail disc uses.

So the failure lies between the ROM loader's jump and boot stage 1, on
silicon only. PCSX2 runs the USA v2.20 ROM; the console's ROM version is a
variable worth recording. Two builds decide what is left:

**`23-boot-probe-big`** is the probe inside a load segment the size of the
game's: 5.76 MB of initialised data in 90 distinct blocks and a 3.4 MB
.bss, checked word by word after the red rung, with a 40 KB frame so the
stack sits where the game's does. Grey after red means the console's loader
did not deliver that segment intact, and the game never ran at all.

| Colour | Meaning |
|---|---|
| Red, then the normal ladder to blue | A game-sized ELF loads and runs here. The game's own code is the difference |
| Red, then grey | The load segment arrived changed or incomplete. The ELF size is the problem, not the code |
| Nothing | Even the red rung did not run: the loader rejected the ELF outright |

**The boot ladder build** of the game (`-DPS2_BOOT_LADDER=ON` on the
il2cpp-port tree) paints the stretch before boot stage 1 with the probe's
register-only trick, so a constructor that hangs or dies is named by the
colour it stops on. Linker wraps count the mutexes, TLS keys and destructor
registrations the constructors make, so third-party code is instrumented
without being edited.

| Colour | Where it stopped |
|---|---|
| Nothing | Before the first constructor: the load, crt0 or libc start-up |
| Dark red | Static constructors have started; the first libstdc++ one |
| Purple, darker | Early in the constructor list: libstdc++ locale, exception and pool set-up |
| Purple, brighter | Later in the list: the libil2cpp metadata, class and thread-pool statics |
| Red | main() entered; GsDevice::init() did not return |
| Orange, then nothing else | GsDevice::init() returned; the first DMA frame never landed |
| Orange, then blue and the normal ramp | The pre-GS stretch is fine on this build |
| White | fatal() before the GS was up |

The ladder build is a diagnostic only: its marks switch the display off, and
it is never what a profile builds.

### 2. First boot

**Start with the probe, not the game.** `samples/23-boot-probe` is a tiny ELF
that climbs the startup sequence one rung at a time and paints the screen at
each rung with the cheapest mechanism that could work there. Copy the ELF alone
to a USB stick and launch it from uLaunchELF; the colour it stops on names the
rung that failed. It needs no disc, no scene and no Unity build.

| Colour | Rung reached |
|---|---|
| Nothing, black | The ELF did not execute, or the EE cannot write GS registers |
| Red | ELF running; BGCOLOR written directly, no DMA, no IOP |
| Orange | IOP reset, SIF RPC back up, buffer-load patch applied |
| Yellow | iomanX and fileXio loaded from EE memory |
| Magenta | Those module loads failed |
| Blue | GS initialised and a frame drawn through the GIF DMA path |
| Green | The frame read back from VRAM as drawn. Success |
| Cyan | Drawn, but the readback disagreed |
| White | GS init failed |


A development build paints the screen a colour as each boot stage completes,
because a console with no serial link has exactly one output device and it is
the TV. The colour the picture stops on names the stage that failed.

| Colour | Stage completed |
|---|---|
| Warped BIOS logo, no colour ever | The GS display was configured but nothing was drawn: the GS or DMA path itself is broken on this hardware |
| Blue | GS initialised, a frame drawn over DMA |
| Green | IOP reset, SIF up, iomanX and fileXio loaded |
| Yellow | Boot scene found on the media |
| Cyan | Container read, parsed, world loaded |
| Magenta | Pads and audio up |
| White | Managed runtime up (`il2cpp_init`) |
| Orange | Assets uploaded, scripts instantiated; the first real frame follows |
| Red | `fatal()` was reached after the last colour you saw |

The first hardware boot stopped on the warped logo, which places the failure
in the disc load (between blue and yellow) on the two candidates in the
catalogue above: the eleven-character scene filename against the CD driver's
8.3 expectation, and the `host:` probe that has no device to answer it.

Expect the first attempt to fail somewhere in the list above. Work in this
order, because it is roughly the order of likelihood and each step rules out
the ones after it:

1. Does it boot at all? A black screen with no output points at the boot ELF
   name and serial, or at the media.
2. Does the log reach the runtime's init line? If not, the IOP module load is
   the first suspect.
3. Does the scene load? A container read failure over `cdrom0:` that works
   over `host:` is a path resolution or a media problem, not an engine one.
4. Does anything draw? A black screen with a healthy log is the GS packet
   audit.
5. Does it draw correctly? Compare against the emulator's golden capture for
   the same scene. Per-tile CRCs make "slightly wrong" a located difference
   rather than an impression.

### 3. Measure

Enable the profiler in the build profile and run the same scenes measured in
the emulator. The numbers to compare are the ones the emulator cannot be
trusted for:

- Dropped flips per hundred frames.
- The pipeline window, which is where DMA timing differences would appear.
- Load times, which the emulator cannot measure at all.
- The 30-minute soak, which is the D4 acceptance criterion and the only test
  that catches a slow leak.

### 4. Record

Every difference found belongs in this file's catalogue and in
`docs/notes/verify-log.md`, with the same discipline as the rest of the
project: what was tried, what failed, and what the measurement was. The
catalogue above is a prediction until hardware turns it into a record.
