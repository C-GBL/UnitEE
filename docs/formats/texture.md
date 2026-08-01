# Texture format

**STUB.** TODO(spec missing: section 10): the texture chunk binary layout is
defined by plan section 10, which is not present in `ps2port.txt`. Do not
invent the container layout. However, the following hardware facts from plan
section 3.3 are already fixed and any spec MUST honour them.

## Hard facts (plan section 3.3)

### Offline swizzling is mandatory

GS VRAM is organised in **pages (8 KB) and blocks (256 bytes)** with
format-specific swizzle patterns. Textures MUST be swizzled offline by the
exporter into the exact VRAM layout for their pixel format; swizzling at
runtime is a waste of EE cycles. The `.p2b` texture payload is therefore a
raw, upload-ready image (sent via GIF IMAGE-mode transfer), not a linear
bitmap.

### Primary pixel formats: PSMT8 and PSMT4

Supported GS formats are PSMCT32 (RGBA8), PSMCT24, PSMCT16/PSMCT16S
(RGBA5551), PSMT8 (8-bit indexed + CLUT), PSMT4 (4-bit indexed + CLUT).
**PSMT8 and PSMT4 are the primary formats for this project** -- with ~1.2 MB
of VRAM left for textures in the baseline video mode, 32-bit textures are a
last resort (a 256x256 PSMT8 texture is 64 KB + 1 KB CLUT; the same texture
in PSMCT32 is 256 KB). The exporter's palettiser quantises source textures to
256- or 16-colour palettes.

### The CSM1 CLUT quirk (pre-swap palettes offline)

CLUT storage has a well-known quirk: for 8-bit indexed textures in CSM1 mode,
the 256-entry palette is stored with its 32-entry blocks reordered -- **blocks
1<->2 and 5<->6 of each group of 8 are swapped**. The exporter MUST emit
palettes pre-swapped so they upload verbatim. Getting this wrong produces
textures with correct shapes and scrambled colours -- which makes it a good
early golden-image test case.

## Open questions for section 10

Chunk header fields (format enum, dimensions, TBW, mip levels if any), CLUT
placement/format (PSMCT32 vs PSMCT16 palette entries), VRAM allocation hints,
and how texture handles are referenced from mesh batches and materials.
