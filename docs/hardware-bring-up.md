# Hardware bring-up

Plan section 9, M13 task 4. This document is the procedure for taking a build
that works in PCSX2 and making it work on a retail console, plus the
catalogue of places the emulator is known to be more forgiving than the
silicon.

**Status: not yet performed.** The development console for this project
failed mid-project and has not been replaced, so everything below is written
from the emulator side and from the hardware's documented behaviour. Nothing
here has been confirmed against a real PlayStation 2, and this file says so
in each place it matters rather than pretending otherwise. Acceptance D3
(boots and plays on fat and slim hardware, from DVD-R and from USB) remains
open, and with it M13's acceptance as a whole.

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
| Cache flush before a DMA read | Tolerates a missing flush in many cases, because it reads the same memory the EE wrote | The DMAC reads physical RAM and does not see dirty cache lines. Every buffer handed to a DMA channel must be flushed first | Handled: the chain flushes itself and every referenced block before kicking, a lesson the GIF path learned at M2. **Unverified on hardware** |
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
| Blue | GS initialised, display configured |
| Green | Boot scene found on the media |
| Yellow | Scene container read into RAM |
| Cyan | Container parsed, world loaded |
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
