# ADR-011: PS2ParticleSystem simulates on the EE and draws through vu_unlit_tex

Status: accepted (M12.5 task 4). Plan 7.2.

## Decision

Particles are a fixed-capacity CPU simulation (8 systems x 128 particles,
32 bytes each) drawn as camera-facing quads through the EXISTING textured
VU1 program at address 300, with the transparent-pass GS state the material
system already defines (alpha 0x44 / additive 0x48, Z test on, Z write
off). No new microprogram, no new batch format, no new material kind.

Billboards are built in WORLD space every frame -- local-space systems
transform their particles by the entity matrix on the EE first -- so there
is exactly one render path and the camera basis never has to be pushed
through a model rotation.

## Why not a VU1 expansion program

A point-to-quad VU program is the "proper" PS2 technique and roughly 4x
the vertex throughput. It is also a new .vsm, a new input format, a new
constants layout and a golden of its own -- for a feature whose budget the
plan fixes at a few hundred quads. 256 particles cost ~18 qwords each to
stage (~4.6K qwords a frame) and well under a millisecond of EE time;
vu_unlit_tex then projects them like any other triangle. M4 taught what a
microprogram costs to get right; that price buys nothing here.

The budget is the contract: `kMaxParticlesPerSystem` is a hard pool bound,
an emitter cannot exceed it, and overflow drops the oldest spawn request
rather than growing anything. If a game genuinely needs thousands of
particles, that is the point at which the VU program earns its
complexity -- and this ADR is where to record superseding it.

## Unity mapping

This is deliberately NOT Unity's ParticleSystem (curves, sub-emitters,
trails, noise, GPU sim). It is `PS2ParticleSystem`, our component, with the
subset plan 7.2 names: rate + burst emission, sphere/cone/box shapes,
constant lifetime/speed, linear size and colour ramps, a gravity
multiplier, looping, world/local space. The validator errors on Unity's
own ParticleSystem and names this replacement. The cone emits along the
transform's +Z; local-space gravity is local -Y by definition.
