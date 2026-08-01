# ADR-003: Where does the geometry pipeline run?

Status: Accepted (plan section 5, ADR-003)

## Context

The GS is a rasteriser only; all vertex transform, lighting, and clipping
happens upstream. The candidates are the EE (software), VU1 (the machine's
"vertex shader", directly connected to the GS via GIF PATH1), or an existing
helper library. The section 3.6 envelope (15k-40k tris/frame at 30 fps) is
the bar.

## Options

- **EE-only software transform, GS via PATH3.** Rejected for the shipping
  path; ~4x slower. **Retained as the M2 bootstrap** so rendering can be
  brought up before any VU assembly exists
  (TODO(spec missing: section 9): M2 definition).
- **VU1 microprogram (PATH1) with EE-side batching.** **Chosen.** The only
  way to hit the section 3.6 numbers.
- **gsKit as the renderer.** Rejected as the architecture, **adopted as a
  reference and for early bring-up**. gsKit is a texture/primitive helper,
  not an engine renderer; its per-primitive model does not batch well.

## Decision

VU1 microprograms per material kind (`vu_unlit`, `vu_lit`, `vu_skin`,
`vu_sprite` -- plan 7.3), fed by an EE-side batcher that builds DMA chains
(in scratchpad, double-buffered) of VIF-unpackable vertex batches. The EE
software-transform path is built first at M2 and kept as a debugging
reference; gsKit is consulted for GS register bring-up.

## Consequences

- VU assembly (`dvp-as`-built `.vsm`, embedded as data blobs and DMA'd to VU1
  micro memory at load) becomes a first-class build artifact
  (`tools/cmake/vu.cmake`, `runtime/src/vu/`).
- The microprogram owns clipping -- there is no hardware clipping. Guard band
  + trivial batch rejection + true near-plane clipping for straddling
  triangles (plan 3.2).
- Batch size is bounded by double-buffered VU1 data memory: 64-96 vertices
  per batch; <=24 bones for skinned batches.
- The exporter's mesh converter must target this exact batch/VIF layout
  offline (docs/formats/mesh-batch.md).
- PATH1 (VU1->GS) vs PATH3 (texture upload) arbitration means texture uploads
  are scheduled at frame boundaries or explicit gaps.
