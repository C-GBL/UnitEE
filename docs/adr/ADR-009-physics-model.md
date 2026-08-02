# ADR-009: The physics model is deliberately not PhysX

Status: accepted (2026-08-02)
Context: plan section 9, M11; scope in 7.1; budget in 15.3.
Related: ADR-002 (managed/native boundary), ADR-007 (subset discipline).

## Context

The plan says M11 is "deliberately minimal and deliberately *not*
PhysX-compatible. The docs must say so plainly." This ADR is that plain
statement, and it records the four decisions that follow from a 4 ms budget
on a 300 MHz in-order CPU with an 8 KB data cache.

The temptation in a port like this is to make physics *look* like Unity's
and hope the differences do not surface. They surface. A game tuned against
PhysX's contact behaviour and retuned against this one is an afternoon; a
game that ships believing they are the same is a bug report from a player.

## Decision 1: static geometry is baked to world space offline; dynamic
bodies are primitives only

Mesh colliders become a world-space BVH over triangles, built by the Editor
(`P2bPhysicsExporter`) and traversed with no per-query transform. That is
where the plan's "static/dynamic split so the broadphase for static geometry
is precomputed" lands: the thing that never moves costs nothing to
broadphase because its acceleration structure is a file.

Dynamic bodies get sphere, box and capsule only. A moving concave mesh needs
per-frame BVH refits and a much more careful narrowphase; Unity discourages
it too (non-convex `MeshCollider` cannot have a `Rigidbody`). The
consequence, stated in the API rather than discovered: a baked collider
cannot move, and `gameObject.AddComponent<BoxCollider>()` at runtime is not
supported.

## Decision 2: one contact point per pair, one correction pass, no
constraint solver

There is no manifold, no friction solver (a fixed tangential damping stands
in), no joints, and no iterative solver. Resting contact settles but may
creep; a tower of three boxes will not stay a tower.

This is the decision most likely to be questioned later, so the reasoning is
explicit: a sequential-impulse solver with manifolds costs roughly an order
of magnitude more than this, and the plan budgets **4 ms** for physics out of
a 33.3 ms frame (15.3). The measured cost of what was built is **71 us
average, 154 us worst** over 2,074 triangles with a driving kart
(`samples/21-kart` on PCSX2) -- so there is headroom, and a future milestone
could spend it. What there is not, is a reason to spend it before a game
needs stacking.

PS2-era games solved this by not relying on rigid-body stacking: gameplay
used character controllers and kinematic bodies. The API points the same way.

## Decision 3: the character controller sweeps; everything else is discrete

`CharacterController.Move` does a swept-capsule collide-and-slide with step
and slope handling. Queries (`Raycast`, `OverlapSphere`) are discrete,
because a ray has no thickness and an overlap test has no motion.

Sweeps advance at half the capsule radius, so nothing thinner than the shape
can be stepped over, then bisect eight times to find the contact fraction.
That is the anti-tunnelling guarantee the acceptance test checks at
**200 m/s -- 6.67 metres per fixed step**, against a wall far thinner than
that. A discrete test at the end position would find the kart cleanly past
the wall and report nothing at all.

Conservative advancement was chosen over a closed-form continuous
capsule-triangle solve because the exact version is a quartic: more edge
cases, worse conditioning, and an error bound that depends on the geometry
rather than on an iteration count we choose.

## Decision 4: contacts are batched; managed code fans them out

The step accumulates a contact list. The managed dispatcher reads it with
one `count` call plus one `get` per contact, then delivers
`OnCollisionEnter`/`OnTriggerStay`/etc. to behaviours entirely in managed
code.

The alternative -- native invoking a managed callback per event -- would
cost a `runtime_invoke` per contact, which M7 measured at 4.7 us against
54 ns for a direct call (87x). A frame with 40 contacts would spend more in
the boundary than in the solver.

Two consequences worth stating:

- The `Collision` object handed to callbacks is **reused**, not allocated
  per event. A 4 MB managed heap with a 2 ms GC budget cannot afford one
  allocation per contact per frame. Games must not cache it.
- Contacts beyond `kMaxContacts` (128) are dropped, and
  `contacts_dropped()` counts them. A silently lost trigger enter is a bug
  the game will blame on itself, so the runtime admits it instead.

## Determinism (plan M11 task 5)

The step is fixed, iteration order is array order, and there is no `double`
anywhere. That makes a build deterministic **against itself** -- same
inputs, same result, every run, asserted by an exact-equality test over 120
steps of six bouncing bodies.

It is explicitly **not** deterministic across builds, nor against the
Editor: the EE's floats are not IEEE 754 (plan 3.1), so a compiler change
can move the last bit. Anything needing cross-machine determinism (replays
shared between consoles, lockstep multiplayer) must not assume it.

## Consequences

Every divergence above is listed in `docs/supported-api.md` as a numbered
conformance deviation, which is the document a porting user actually reads.
The Editor-side validator (plan 13.5) should eventually flag the API calls
that have no equivalent here -- joints, `AddTorque`, runtime collider
creation -- rather than letting them fail at link time. That is tracked as
an open item, not solved here.
