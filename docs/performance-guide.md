# Performance guide

Seeded from `ps2port.txt` sections 3.2, 3.3, and 3.6. Budgets will be
finalized when the budgets section exists (TODO(spec missing: section 15)).

## Reference performance envelope (plan section 3.6)

Realistic targets for a homebrew engine of this kind, at 30 fps, 512x448:

| Metric | Conservative target | Stretch |
|---|---|---|
| Triangles/frame | 15,000 | 40,000 |
| Draw batches/frame | 150 | 400 |
| Unique textures resident | 40 x 128^2, PSMT8 | 80 |
| Skinned characters | 3 @ 1,500 tris, 24 bones | 6 |
| EE budget/frame | 33 ms total; <=12 ms game logic (managed) | <=8 ms |
| Managed heap | 4 MB | 6 MB |

## Hardware constraints you must internalise

### Floating point: `double` is a trap, `float` is not IEEE 754

- The EE FPU is **single-precision only and not IEEE 754**. There is no
  double-precision hardware at all.
- Every C# `double` operation compiles to a libgcc soft-float call --
  **20-100x slower than `float`**. This is the single biggest managed-code
  performance trap. The build validator warns on `double` in hot paths.
  Beware of implicit `double`: `Math.*` (use `Mathf`), `double` literals
  (`0.5` vs `0.5f`), and APIs returning `double`.
- `float` has **no NaN and no infinities**: overflow saturates (`0/0` yields
  Fmax = `0x7FFFFFFF` as a float pattern), `inf + -inf` yields `0.0`,
  denormals flush to zero, rounding is toward zero. `float.IsNaN(x)` after a
  division by zero returns `false`. This is a permanent, documented
  conformance deviation -- do not write algorithms that depend on NaN/Inf
  propagation.

### Texture budget reality (plan section 3.3)

- The GS has **4 MB of VRAM total** -- framebuffers, Z-buffer, AND textures.
  The baseline video mode (512x448 interlaced NTSC, PSMCT32 double-buffered
  colour + PSMZ24 Z) consumes ~2.75 MB, leaving **~1.2 MB for textures**. The
  aggressive alternative (PSMCT16 colour + PSMZ16S Z, ~1.4 MB) leaves ~2.6 MB.
- **You will live in PSMT8 and PSMT4** (indexed + CLUT). A 256x256 PSMT8
  texture is 64 KB + 1 KB CLUT; the same texture in PSMCT32 is 256 KB and
  roughly nine of them fill the entire machine.
- Textures are swizzled offline by the exporter into the GS page (8 KB) /
  block (256 byte) layout; runtime swizzling wastes EE cycles. See
  `docs/formats/texture.md` for the layout and the CSM1 CLUT quirk.
- Texture uploads (PATH3) mid-frame stall VU1 drawing (PATH1); schedule
  uploads at frame boundaries or explicit gaps.

### Batch sizes (plan section 3.2)

- VU1 data memory is 16 KB, used **double-buffered** (VIF unpacks batch N+1
  while the microprogram processes batch N). Buffer size drives maximum batch
  size: realistically **64-96 vertices per batch** with position + normal +
  UV + colour.
- There is **no hardware clipping**; the microprogram does guard-band and
  near-plane REJECTION per triangle (a triangle with any vertex behind the
  near plane or outside the 4095-unit guard band is dropped whole; true
  clipping of straddling triangles is the recorded follow-up). The exporter
  subdivides triangles to 1.5 world-unit edges so the holes that leaves stay
  below the screen edge for a camera at least that far above the surface it
  looks along.
- Skinned batches carry a matrix palette in VU1 data memory: **<=24 bones per
  batch**.

### Memory and CPU

- 32 MB RDRAM total (D4 target: peak <=30 MB); high latency -- cache misses
  are brutally expensive (16 KB I-cache, 8 KB D-cache).
- The 16 KB scratchpad at `0x70000000` is the single most valuable resource on
  the machine: single-cycle, DMA-addressable. Use it for DMA chain/packet
  assembly and hot working sets.
- Build the whole frame's geometry as one DMA chain (double-buffered: EE
  builds frame N+1 while the DMAC walks frame N).
- Quantising positions to `V4-16` VIF unpack halves geometry bandwidth for a
  modest precision cost.
- Disc seeks are ~100 ms; file placement order on the ISO matters (mkps2iso
  controls LBA order). Group assets by load unit.
