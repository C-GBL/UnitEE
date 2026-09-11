# ADR-012: Lighting, shadows and sky on the GS

Status: accepted (M14, 2026-09-11)

## Context

The engine lit every scene with one directional light fixed at export and
an ambient constant compiled into the renderer. There was no sky beyond the
clear colour and no shadow of any kind. Unity projects assume point lights
that move, a skybox, and something under the player's feet. The hardware
has a per-vertex lighting stage on VU1 with three light slots and an
ambient term per batch, no shadow maps, no shaders, and a GS whose fill
rate is the budget that matters.

## Decision

**Lights are chosen per object, per frame, from a scene table.** Every
Directional, Point and Spot light exports and is mirrored by a managed
`Light`. Each drawn object takes the three brightest lights that reach its
bounding sphere: a directional light as its transform's forward, a point
or spot light as a directional light aimed at the object's centre with a
`(1 - d/reach)^2` falloff (and a cone term for spots). Directions and
positions are read from the light entities' world matrices at draw time,
which is what makes them dynamic. The VU programs are unchanged.

Not chosen: per-vertex point lights in the microprograms. They would cost a
subtract, a normalise and a multiply per vertex per light, roughly doubling
the lit path, for a difference that shows only on meshes larger than a
light's range. The era approximated exactly as above, and static batching
keeps merged meshes small enough that the approximation holds.

**Shadows are blobs and planar projections.** A `PS2Shadow` draws a
16-triangle fan of black with an alpha ramp on the ground a downward ray
finds, faded by height. Mode Projected also redraws the object's lit
meshes and skinned renderers through a planar projection along the
brightest directional light onto that ground plane, with the light
constants zeroed so the lit programs output black, blended as
`Cd * (1 - strength)` through the GS FIX alpha, Z-tested and not Z-written.
The object is drawn twice, which is why it is opt-in per object.

Not chosen: shadow maps (a depth render-to-texture and a compare the GS
cannot do in one pass), stencil volumes (no stencil buffer in this
pipeline, and the fill cost of volumes on the GS is prohibitive at 512x448
with a 4 ms geometry budget). Projected shadows fall only on the ground
found by the ray; they do not fall on other objects.

**The sky is six baked faces on a cube that follows the camera.** The
exporter renders Unity's skybox, whatever shader it uses, through a
90-degree camera along each axis into six textures, and adds a small cube
whose entity is flagged to draw at the camera's position, first, with no
Z write and an always-passing depth test. Clamp addressing keeps the face
edges from wrapping. A radius of 4 units keeps the cube's triangles small
enough, after the exporter's subdivision, that near-plane rejection never
opens a hole at the screen edge.

Not chosen: a procedural gradient (cannot reproduce an authored sky), a
single panoramic dome (one texture too large for VRAM at a usable
resolution), or a distant cube (large triangles cross the near plane and
are rejected whole; see ADR-003).

## Consequences

- A scene's lighting changes with its lights, at no VU cost; large lit
  objects read a point light as a whole, which the docs say plainly.
- Shadows cost fill rate, not memory. A projected character doubles its
  draw; the budget report counts shadow draws.
- The skybox costs six textures of VRAM (96 KB at 128x128) and six draws.
- The MATL flags, the entity flags, the Light payload and the Camera
  payload grew, all backward compatible (old readers stop at the old sizes).
