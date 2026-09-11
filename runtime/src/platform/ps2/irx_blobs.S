/* Embedded IOP modules (plan section 3.5).
 *
 * Modern ps2sdk's newlib routes ALL file I/O through fileXio, which is not a
 * ROM module: without iomanX.irx + fileXio.irx loaded on the IOP, every
 * open() fails instantly with no useful error. PCSX2's fastboot provides only
 * the ROM modules, so the runtime carries these two and loads them at init.
 *
 * The .incbin paths are resolved by the assembler's include search path,
 * which the build points at $PS2SDK/iop/irx (see runtime/CMakeLists.txt).
 */
    .section .rodata
    .balign 64
    .global iomanx_irx_start
iomanx_irx_start:
    .incbin "iomanX.irx"
    .global iomanx_irx_end
iomanx_irx_end:

    .balign 64
    .global filexio_irx_start
filexio_irx_start:
    .incbin "fileXio.irx"
    .global filexio_irx_end
filexio_irx_end:

/* M10. audsrv is the sanctioned starting point for audio (plan 9, M10 task
 * 1); it needs rom0:LIBSD, which is on the console already. The pad and
 * memory-card stacks are the freesio2/freepad and mcman/mcserv pairs --
 * embedding them keeps a booted ELF self-contained, exactly like the file
 * I/O modules above. */
    .balign 64
    .global audsrv_irx_start
audsrv_irx_start:
    .incbin "audsrv.irx"
    .global audsrv_irx_end
audsrv_irx_end:

    .balign 64
    .global sio2man_irx_start
sio2man_irx_start:
    .incbin "freesio2.irx"
    .global sio2man_irx_end
sio2man_irx_end:

    .balign 64
    .global padman_irx_start
padman_irx_start:
    .incbin "freepad.irx"
    .global padman_irx_end
padman_irx_end:

    .balign 64
    .global mcman_irx_start
mcman_irx_start:
    .incbin "mcman.irx"
    .global mcman_irx_end
mcman_irx_end:

    .balign 64
    .global mcserv_irx_start
mcserv_irx_start:
    .incbin "mcserv.irx"
    .global mcserv_irx_end
mcserv_irx_end:

/* errno, aligned.
 *
 * ps2sdk's libkernel defines errno WEAK in errno.o, in a .data section with
 * byte alignment (objdump: 2**0), and its __errno() -- the accessor behind
 * newlib's errno macro -- returns that symbol's address. The linker drops a
 * byte-aligned section wherever the previous input ended: in the game, one
 * byte after a bool, at 0x005ab0e1. Every errno read or write was then a
 * 4-byte access at an odd address. PCSX2's recompiler performs it anyway;
 * its interpreter and the R5900 raise an Address Error, and the console's
 * boot died before its first colour (verify-log, 2026-09-09). A strong,
 * aligned definition here takes the symbol for every ELF that links the
 * runtime, since libps2ur.a precedes libkernel.a on the link line and a
 * strong definition beats the weak one regardless.
 */
    .data
    .balign 16
    .global errno
    .type errno, @object
    .size errno, 4
errno:
    .word 0
