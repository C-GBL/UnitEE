# runtime/iop -- custom IOP modules (.irx)

Stub directory. Custom `.irx` IOP modules come later (plan section 3.5).

The IOP (R3000A, 2 MB RAM) owns CDVD, USB, controllers, memory cards, sound,
and networking; the EE talks to it over SIF RPC + DMA. Baseline plan uses stock
PS2SDK modules (`audsrv.irx`, `mcserv.irx`/`mcman.irx`, `sio2man.irx` +
`padman.irx`); a custom audio IRX is the endgame if tight mixing control is
needed.

Modules here are built with the IOP compiler from the ps2dev toolchain
(`mipsel-none-elf-gcc`, `[VERIFY exact IOP triple in your install]`, plan
section 4.1), rooted at `%PS2DEV%` (default `C:/Users/Ash/ps2dev`).

TODO(spec missing: section 9): which milestone introduces the first custom IRX.
