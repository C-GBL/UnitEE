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
    .balign 16
    .global iomanx_irx_start
iomanx_irx_start:
    .incbin "iomanX.irx"
    .global iomanx_irx_end
iomanx_irx_end:

    .balign 16
    .global filexio_irx_start
filexio_irx_start:
    .incbin "fileXio.irx"
    .global filexio_irx_end
filexio_irx_end:
