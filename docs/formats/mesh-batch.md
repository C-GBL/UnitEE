# Mesh batch format

**STUB.** TODO(spec missing: section 10): the mesh batch binary layout is
defined by plan section 10, which is not present in `ps2port.txt`. Do not
invent the format.

Constraints already fixed by sections 3 and 7 that the spec must satisfy:

- Batches are sized for double-buffered VU1 data memory (16 KB total):
  realistically **64-96 vertices per batch** with position + normal + UV +
  colour (plan 3.2).
- Vertex data is stored ready for VIF unpack (`V4-32`, `V3-32`, `V4-16`,
  `V4-8`, `S-32`, ...); positions may be quantised to `V4-16` to halve
  bandwidth (plan 3.4). The exporter's tri-stripper and VU1 batcher produce
  this layout offline (plan section 6).
- Each batch is associated with one material kind (plan 7.3), which selects
  the VU1 microprogram and GS state.
- Skinned batches (`vu_skin`) carry a matrix palette of **<=24 bones** (plan
  7.3).
- Batches should be emittable directly into a DMA chain (`cnt`/`next`/`ref`
  descriptors) with minimal EE-side fixup (plan 3.4).
