# The `.p2b` container (v1)

Implements plan section 10. Written BEFORE the writer and reader, per M5
task 1; both sides are written against this document and the fuzz test
targets the reader.

Design constraints (10.1): zero-copy (sections are loaded whole and used in
place), 2048-byte alignment for section payloads, little-endian everywhere,
versioned with a loud reject on mismatch.

All offsets are from the start of the file. All structures are packed,
little-endian, and sized as written -- no implicit padding.

## File layout (10.2)

```
offset  size    field
0       4       magic 'P2BC' (bytes 50 32 42 43)
4       2       version_major   (this spec: 1)
6       2       version_minor   (this spec: 0)
8       4       total_size      (bytes, whole file)
12      4       section_count
16      4       flags           (bit0 = compressed sections present; v1: 0)
20      12      reserved (zero)
32      n*32    section table
...             payloads, each starting at a 2048-aligned offset
```

Section table entry (32 bytes):

```
u32 type          fourcc, see below
u32 offset        payload start (2048-aligned)
u32 size_on_disc  payload bytes
u32 size_in_ram   == size_on_disc in v1 (no compression)
u32 checksum      CRC-32 (reflected, poly 0xEDB88320) of the payload
u32 flags         0 in v1
u64 name_hash     FNV-1a 64 of the source asset name; 0 if unnamed
```

Section types present in v1: `MESH`, `TEX ` (trailing space), `SCEN`,
`MATL`. Multiple sections of the same type are ordered; indices in other
sections refer to that order (e.g. "texture 2" = the third `TEX ` section).
Types reserved by the plan for later milestones: `CLUT`, `ANIM`, `SKEL`,
`AUDI`, `FONT`, `STRT`, `SCPT`, `PHYS`.

## MESH section

Serves plan 10.3's requirement -- "stored already batched for VU1; the
runtime never reorganises geometry" -- in the SDK-chain form the runtime
draws with (see `runtime/include/ps2ur/gs_batch.h`): each batch is a
`BatchBlock` the chain references zero-copy.

```
MeshHeader {
    u32 batch_count
    u32 material_index      // into the MATL array
    f32 bounds_center[3]
    f32 bounds_radius       // object-space bounding sphere; per-batch
                            // spheres arrive with culling at M8
}
BatchDesc[batch_count] {
    u32 offset_qwords       // from the start of this section
    u32 vert_qwords
    u32 vertex_count
    u32 vert_dest           // VU address vertices unpack to (10 / 18)
}
...16-byte-aligned batch blobs, each: [GIF tag qw][count qw][vertex qws]
```

Vertex layouts by material kind (see MATL): unlit 2 qw (pos, colour),
unlit-textured 3 qw (pos, stq with q pre-set to 1, colour), lit 3 qw (pos,
normal, colour). Positions are `V4-32` floats with w = 1.

**Recorded deviation from plan 10.3:** v1 stores raw qword payloads consumed
through ref-tag unpacks, not a pre-built VIF-code stream, and positions are
V4-32 float, not V4-16 quantised, with triangle lists rather than strips.
The pre-built-VIF/quantised/stripped form is the optimisation path once M8's
render queue exists; the section format holds either (the descs say where
data is, not how it was encoded).

## TEX section

Self-contained indexed texture:

```
TexHeader {
    u32 width, height       // powers of two
    u32 format              // GS PSM code; v1 emits PSMT8 (0x13)
    u32 clut_entries        // 256 for PSMT8
}
u32 clut[clut_entries]      // PSMCT32 entries, alpha already 0..128,
                            // ALREADY in CSM1 storage order
u8  indices[width*height]   // RASTER order -- the GS transfer engine
                            // swizzles in hardware (verify-log 2026-08-01)
```

## MATL section (v2 records, M8 -- BREAKING vs the 24-byte v1 record)

```
Material[count] {           // 48 bytes each; reader rejects other strides
    u32 kind                // 0 unlit, 1 unlit-textured, 2 vertex-lit,
                            // 3 lit-alpha, 4 cutout, 5 additive,
                            // 6 vertex-lit-fog (section 7.3 kinds)
    u32 texture_index       // into TEX order; 0xFFFFFFFF if none
    f32 tint[4]             // reserved, (1,1,1,1)
    u64 gs_test             // TEST_1 register value; 0 = device default
    u64 gs_alpha            // ALPHA_1 register value; used when blend set
    u32 flags               // bit0 zwrite, bit1 blend, bit2 transparent-pass
    u32 pad
}
```

The GS state is DATA (M8 task 5): the runtime memcpys TEST/ALPHA into the
command stream between chain kicks. Recorded deviation from the plan's
"register blocks precomputed at export": ZBUF carries a VRAM base pointer
only the runtime knows, so Z-write travels as the flag bit and the runtime
composes ZBUF itself. PRIM bits that belong to the material (ABE for blend
kinds, FGE for fog kinds) are baked into each batch's GIF tag in MESH.

## SCEN section (10.4)

```
SceneHeader {
    u32 entity_count
    u32 component_count
    u32 name_table_offset   // 0 in v1 (no STRT yet)
}
Entity[entity_count] {
    i32 parent              // < own index, or -1 (parent-before-child order)
    f32 pos[3]
    f32 rot[4]              // quaternion x,y,z,w
    f32 scale[3]
    u32 name_hash
    u16 layer
    u16 tag
    u32 flags
    u32 component_first     // into ComponentRef[]
    u16 component_count
    u16 pad                 // zero; keeps the struct 4-byte aligned (the
                            // plan's struct leaves this implicit)
}
ComponentRef[component_count] {
    u16 type_id             // 1 MeshRenderer, 2 Camera, 3 DirectionalLight,
                            // 4 Script (M7)
    u16 pad
    u32 data_offset         // from the start of this section
}
```

Component payloads:

```
MeshRenderer     { u32 mesh_index; u32 material_index }
Camera           { f32 fov_radians; f32 znear; f32 zfar }        // 12-byte v1
Camera (M8, 64B) { ...v1...; u32 orthographic; f32 ortho_size;
                   f32 viewport[4];          // x,y,w,h in [0,1]
                   u32 clear_flags;          // 1 colour+depth, 2 depth only
                   u32 clear_rgb;            // r | g<<8 | b<<16
                   u32 layer_mask;
                   u32 fog_enabled; u32 fog_rgb; f32 fog_near; f32 fog_far }
DirectionalLight { f32 dir[3]; f32 colour[3] }   // dir points FROM the light
Script (M7)      { u32 scrp_offset }             // into the SCRP section
```

Readers accept the 12-byte camera and default the M8 tail (old runtimes
tolerate new exporters and vice versa).

## SCRP section (M7)

Packed NUL-terminated managed type names, ASCII, in the form
`"Full.Type.Name, AssemblyName"`. Script component payloads reference byte
offsets into this section; the reader must prove the NUL lies inside the
section before keeping a pointer.

## Reader obligations

The reader must treat every field as hostile (M5 task 1: fuzzed): validate
magic/version, that the table fits in the file, that every payload
[offset, offset+size) lies inside the file, that payload alignment is 2048,
and verify checksums. Component/batch offsets are validated against their
section's bounds before use. A malformed file produces a clean failure,
never a wild pointer.
