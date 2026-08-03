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
                            // 4 Script (M7), 5 SkinnedMeshRenderer (M9),
                            // 6 Rigidbody (M11), 7 Animator (M12.5),
                            // 8 AudioSource, 9 AudioListener,
                            // 10 PS2ParticleSystem, 11 UIElement (M12.5)
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
SkinnedMeshRenderer (M9, 16B) {
                   u32 skms_index;
                   u32 material_index;       // 0xFFFFFFFF = the mesh's own
                   u32 animator_group;       // see below
                   u32 controller_index }
Rigidbody (M11, 16B) {
                   f32 mass; f32 linear_damping; f32 angular_damping;
                   u32 flags }               // bit0 gravity, bit1 kinematic,
                                             // bit2 freeze rotation
Animator (M12.5, 8B) {
                   u32 controller_index; u32 layers_baked }
AudioSource (M12.5, 24B) {
                   u32 clip;                 // SND record index; -1 none
                   f32 volume;
                   u32 flags;                // bit0 playOnAwake, bit1 loop,
                                             // bit2 spatial (3D pan+atten)
                   f32 min_distance; f32 max_distance;
                   u32 priority }            // NATIVE scale, higher wins:
                                             // the exporter wrote 256-unity
AudioListener (M12.5, 4B) { u32 pad }        // one per scene; loader keeps
                                             // the first
PS2ParticleSystem (M12.5, 64B, ADR-011) {
                   u32 texture;              // TEX index; -1 untextured
                   u32 flags;                // bit0 looping, bit1 playOnAwake,
                                             // bit2 additive, bit3 world-space
                   f32 emission_rate; u32 burst_count;
                   u32 shape;                // 0 sphere, 1 cone(+Z), 2 box
                   f32 shape_a, shape_b, shape_c;
                   f32 lifetime, speed, size_start, size_end;
                   u32 colour_start, colour_end;  // RGBA8
                   f32 gravity;              // multiplier of 9.81 down
                   u32 max_particles }       // clamped to the runtime's 128
UIElement (M12.5 task 5, 84B) {
                   u32 kind;                 // low8: 0 rect, 1 image, 2 text;
                                             // bits 8-15 managed kind
                                             // (Image/RawImage/Text);
                                             // bits 16+ role (1 button,
                                             // 2 slider)
                   f32 x, y, w, h;           // screen px, top-left, BAKED
                                             // RectTransform resolution
                   u32 colour;               // RGBA8, alpha 0..0x80
                   u32 texture;              // TEX index; -1 untextured
                   i32 link;                 // slider: fill element index
                   u32 text_scale;           // baked-font integer scale
                   u8  text[48] }            // NUL text; for a slider these
                                             // carry f32 value, f32 max fill
                                             // width instead
```

Readers accept the 12-byte camera and default the M8 tail (old runtimes
tolerate new exporters and vice versa).

`animator_group` says which CHARACTER a skinned renderer belongs to.
Renderers sharing a group share one `anim::Animator`; different groups
animate independently. The runtime cannot infer this, and both mistakes are
real: a character imported from a DCC tool is many renderers over one
skeleton (Unity-chan is 19, one per material) and they must show the same
pose, while several characters built from the same rig share skeleton and
controller yet must not. The exporter groups by the `Animator` component
that drives each renderer -- the same question Unity answers with
`GetComponentInParent<Animator>()` -- falling back to the transform root.

This field occupied the same offset in M9, where it was written as 0 and
ignored (the skeleton index, which the runtime takes from the SKMS instead).
A `.p2b` written before M12.5 that contains more than one character
therefore reads as ONE animator and must be re-exported.

## SCRP section (M7)

Packed NUL-terminated managed type names, ASCII, in the form
`"Full.Type.Name, AssemblyName"`. Script component payloads reference byte
offsets into this section; the reader must prove the NUL lies inside the
section before keeping a pointer.

## SKEL section (M9)

```
u32 bone_count
u32 pad[3]
Bone[bone_count] {                  // 112 bytes each
    i32 parent                      // < own index, or -1 (see below)
    u32 name_hash
    f32 inverse_bind[16]            // mesh space -> bone space (Unity bindpose)
    f32 rest_pos[3]
    f32 rest_rot[4]                 // quaternion x,y,z,w
    f32 rest_scale[3]
}
```

Bones are written **parent-before-child**, the same rule the entity table
follows and for the same reason: the runtime composes bone matrices in one
linear pass. Unity's `SkinnedMeshRenderer.bones` carries no such guarantee,
so the exporter topologically sorts and permutes the bind poses and every
weight index to match.

## ANIM section (M9), one per clip

```
ClipHeader {
    u32 name_hash
    f32 duration                    // seconds
    u32 track_count
    u32 flags                       // bit0 loop
    u32 key_count                   // total keys in this clip
    u32 pad[3]
}
Track[track_count] {                // 16 bytes each
    u16 bone
    u8  channel                     // 0 translation, 1 rotation, 2 scale
    u8  pad
    u16 key_count
    u16 pad2
    u32 key_first                   // index into the key stream
    f32 quant_scale                 // dequantise: raw * quant_scale / 32767
}
Key[key_count] {                    // 12 bytes each, one stride for all channels
    u16 time_norm                   // time / duration * 65535
    u16 pad
    i16 v[4]                        // translation/scale use v[0..2]
}
```

Rotations are 4x16-bit quaternion components with `quant_scale` 1.0;
translations and scales are 16-bit with a per-track scale, so a track that
moves a few centimetres keeps full precision instead of spending its range
on a world-sized bound. Time resolves to `duration / 65535` -- 76 us on a
5-second clip.

Keys are keyframe-reduced at export: a sample survives only if dropping it
would move the reconstructed curve past a tolerance somewhere between its
surviving neighbours (rotations compare by quaternion dot product, since
their components are not independent).

## CTRL section (M9), the baked state machine

```
u32 state_count, transition_count, param_count, pad
State[state_count] {                // 16 bytes
    u32 name_hash
    u16 clip
    u16 pad
    f32 speed
    u32 flags                       // bit0 loop
}
Transition[transition_count] {      // 16 bytes
    u16 from, to
    f32 duration                    // crossfade seconds
    u8  condition                   // 0 exit-time, 1 bool-true, 2 bool-false,
                                    // 3 float>, 4 float<, 5 trigger
    u8  param
    u16 pad
    f32 threshold
}
u32 param_hash[param_count]
```

## SKMS section (M9), one per skinned mesh

Skinned meshes carry their own section type rather than extending MESH:
their batches need per-batch bone tables, and 16 triangles per batch means a
character needs ~100 batches where a rigid mesh needs one or two.

```
SkinnedMeshHeader {                 // 32 bytes
    u32 batch_count
    u32 material_index
    f32 bounds_center[3]
    f32 bounds_radius
    u32 skeleton_index
    u32 flags                       // bit0 = textured (M12.5); was pad,
                                    // always written 0 before, so old files
                                    // read as untextured. Unknown bits are a
                                    // load error, not a warning.
}
BatchDesc[batch_count] { u32 offset_qw, vert_qw, vcount, vdest }   // vdest = 114
BoneTable[batch_count] {            // 64 bytes, fixed stride
    u32 count                       // <= 24
    u16 bones[24]                   // indices into the skeleton
    u16 pad[6]
}
(pad to 16)
Blob[batch_count] { GifTag; count qw; Vertex[vcount] }
```

Vertex, 5 quadwords (flags bit0 clear) or 6 (set):

```
+0 f32 position[4]      // w = 1
+1 f32 normal[4]        // w = 0
+2 f32 colour[4]        // 0..255, alpha in PS2 range (0x80 opaque)
+3 i32 palette_offset[4] // bone_slot * 4 -- INTEGERS, read by ILW
+4 f32 weight[4]        // sums to 1
+5 f32 texcoord[4]      // textured only: (u, 1-v, 1, 0), v pre-flipped to
                        // GS raster orientation like the rigid MESH path
```

The stride decides the microprogram (vu_skin at 1300 vs vu_skin_tex at
1500) and the batch ceiling: 48 vertices (16 triangles) untextured, 42 (14)
textured -- both are the largest multiple of 3 whose unpack stays inside
the 8-bit VIF NUM limit of 255 qwords. Textured batch tags carry NREG=3,
regs ST+RGBAQ+XYZ2, PRIM.TME set; untextured stay NREG=2, RGBAQ+XYZ2.

`palette_offset` holds *local slot* indices (already multiplied by the
4-quadword matrix stride), not skeleton bone indices: the microprogram adds
the palette base and loads directly. Unused influences point at slot 0 with
weight 0, because the microprogram always reads four matrices and the
address has to stay inside the palette.

Batches are produced by the greedy partitioner described in
`runtime/include/ps2ur/anim.h`; the runtime re-validates every table it
loads (size, range, duplicates), so an exporter bug fails at load rather
than drawing a character inside out. When a character's whole skeleton fits
one palette -- the usual case -- every batch is given the same table so the
palette uploads once per character instead of once per batch.

## PHYS section (M11), baked collision

One per scene. Carries the primitive colliders and a world-space BVH over
the static mesh-collider triangles. `PHYS` is the type plan 10.2 reserved.

Header (32 bytes), all `u32`. Offsets are relative to the SECTION PAYLOAD,
so the section relocates freely:

```
+0  collider_count
+4  node_count
+8  triangle_count
+12 vertex_count
+16 colliders_offset
+20 nodes_offset
+24 triangles_offset
+28 vertices_offset
```

Collider record (48 bytes):

```
+0  u32 kind          0 sphere, 1 box, 2 capsule, 3 mesh
+4  u32 flags         bit0 is_trigger, bit1 enabled,
                      bits2-3 capsule axis (0 X, 1 Y, 2 Z)
+8  u32 layer         0..31
+12 i32 entity        scene entity index, or -1 for a purely static collider
+16 f32 center[3]     local offset from the entity origin
+28 f32 half_extents[3]  box: half size. sphere/capsule: [0] is the radius.
+40 f32 height        capsule total height, INCLUDING both caps (Unity's)
+44 u32 reserved      0
```

The record deliberately has no Rigidbody index: which body drives a collider
is runtime state the file cannot know, so the loader writes -1 and whoever
creates the body wires it up.

BVH node (32 bytes), and the reason it is exactly 32: the EE's data cache is
8 KB, and a traversal that touches one cache line per node is the difference
between a query that fits the 4 ms budget and one that does not.

```
+0  f32 bmin[3]
+12 u32 first     leaf: first triangle index. internal: LEFT child index.
+16 f32 bmax[3]
+28 u32 count     0 means internal; the right child is always first + 1
```

Triangle (8 bytes). Vertices are shared and indexed, which is what keeps a
collision mesh smaller than the render mesh it came from:

```
+0 u16 v0
+2 u16 v1
+4 u16 v2
+6 u8  layer
+7 u8  flags     0 in v1
```

Vertices are `f32[3]`, world space, 12 bytes each. Baking to world space is
what lets the runtime traverse with no per-query transform; it also means
static geometry cannot be moved at runtime, which is the definition of
static.

Because triangle indices are `u16`, a single scene's collision mesh is
capped at 65,536 vertices; the loader refuses more rather than wrapping.
Leaves hold at most 4 triangles (`kBvhLeafSize`), and the builder caps depth
at 32 so coincident triangles -- a common export artefact, since they never
split on any axis -- cannot recurse forever.

The runtime uses the nodes, triangles and vertices IN PLACE (zero copy), so
the payload must be 16-byte aligned; the container's 2048-byte payload
alignment covers it, and the loader checks anyway. `validate_bvh` re-derives
that every child index is in range, every leaf's triangles lie inside the
array, every parent box contains its children, and every triangle is
reachable from exactly one leaf -- so a bad exporter fails at load rather
than producing collision that is quietly wrong in one corner of the level.

## Reader obligations

The reader must treat every field as hostile (M5 task 1: fuzzed): validate
magic/version, that the table fits in the file, that every payload
[offset, offset+size) lies inside the file, that payload alignment is 2048,
and verify checksums. Component/batch offsets are validated against their
section's bounds before use. A malformed file produces a clean failure,
never a wild pointer.
