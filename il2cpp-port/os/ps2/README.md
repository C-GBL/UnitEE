# il2cpp-port/os/ps2 -- the PS2 OS-layer overlay

Files in this directory are overlaid by `apply.py prepare` into the staged
copy at `build/il2cpp/libil2cpp/os/ps2/` (path-preserving). Everything here
is a NEW file of ours. Modifications to existing Unity files must be
patches in `../../patches/`, never edited copies stored here.

(This README itself gets copied into the staged tree; that is harmless and
keeps the overlay machinery exercised while the directory is otherwise
empty.)

## What will live here

`libil2cpp` abstracts its platform layer behind classes under `os/`
selected by `IL2CPP_TARGET_*` switches in `il2cpp-config.h` and
`IL2CPP_USE_GENERIC_*` fallbacks (plan section 1.2 item 2). Adding
`IL2CPP_TARGET_PS2` is "a supported-shaped operation". The PS2
implementations planned for this overlay:

- `Thread`, `Mutex`, `Semaphore` -- single-threaded-first (below)
- `Time` -- EE COP0 Count register / ps2sdk clock services
- `File`, `Directory` -- routed through the ps2ur io layer (CDVD on disc,
  `host:` filesystem under ps2link/PCSX2)
- `Console` -- EE SIO / ps2link tty via the ps2ur log sink
- `Memory` -- fixed arenas from the ps2ur allocator; no virtual memory,
  no mmap (32 MB RDRAM total, plan section 3.1)
- `Environment` -- stubs (no environment variables on the PS2; matches
  `NO_GETENV` in the bdwgc draft, see `../../bdwgc/`)

Where `IL2CPP_USE_GENERIC_*` fallbacks in `il2cpp-config.h` already cover a
class adequately for a single-threaded console target, we prefer enabling
the generic path (a patch) over writing a PS2 file (an overlay).

## Strategy: single-threaded first

The entire runtime is brought up on one EE thread. `Thread`/`Mutex`/
`Semaphore` are cooperative no-op implementations that satisfy the
interfaces; managed `System.Threading` use beyond the main thread is
rejected by the Editor-side validator. This keeps the GC trivially
stop-the-world (see `../../bdwgc/gcconfig-ps2-draft.h`) and defers the
ps2sdk `CreateThread` question until something actually needs it.

## Status

Nothing implemented yet. The class-by-class contract, the bring-up order,
and the exact `il2cpp-config.h` wiring are owned by the IL2CPP port spec,
which is not present in the plan file. TODO(spec missing: section 11)
