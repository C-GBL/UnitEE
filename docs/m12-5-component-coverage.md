# M12.5 -- Component coverage

**Goal:** make plan section 7.1 true. Every component the scope contract
promises is authored in the Editor, carried by the exporter, exposed by the
shim with Unity's semantics, and driven by the runtime -- with an on-target
assertion that it does its job.

Inserted between M12 and M13 by ADR-010. `ps2port.txt` is unchanged; this
adds no scope, it closes what section 7.1 already promised.

## The shape of the problem

Not "write these features". Most of them are written. The M9 animation
runtime passes golden parity against Unity's own sampler; the M10 audio
mixer runs 24 voices with streamed music and zero underruns; the memcard
store is already `PlayerPrefs`-shaped. What is missing is the path from a
Unity asset to that runtime, and a managed class a script can hold.

That is the same defect M12 shipped with `Rigidbody`, which is worth stating
plainly because it is the thing to design against: **a component can be
supported by every layer and still not exist.** The per-layer test suites
all pass, because each layer is correct. Only an end-to-end assertion --
author it in the Editor, build it, run it on target, check it did something
-- can see the gap.

So every task below ends with an on-target token, not a unit test.

## Definition of done for a component

A component is done when all six hold. Anything less and the validator must
keep reporting it as dropped (see `PS2ContentValidator.ExportedComponents`).

1. Authored in the Editor as the ordinary Unity component, with no PS2-only
   companion component required.
2. Carried by `P2bSceneExporter` from a scene walk -- not from a bespoke
   export menu, and not behind a `Pending*` static.
3. Round-tripped by a host test written from the format spec, independently
   of the C# writer.
4. Exposed by `PS2.UnityShim` with Unity's exact semantics, or absent
   (ADR-007). Every member does something.
5. Reachable: `GetComponent<T>()` returns it, and `AddComponent<T>()`
   produces a bound one.
6. Asserted on target by a sample that fails loudly if the component does
   nothing.

---

## Task 1 -- `Animator` + `SkinnedMeshRenderer` from real Unity assets

The M9 runtime is complete and unreachable. `PS2SkinExportMenu` builds a
procedural 24-bone rig in code; there is no path from an imported character.

1. **Skeleton export** from `SkinnedMeshRenderer.bones` + `rootBone` +
   `sharedMesh.bindposes` -> SKEL. Bone order is the mesh's own, because
   `boneWeights` index it.
2. **Clip export** from `AnimationClip` assets: sample each curve at the
   clip's frame rate through Unity's own evaluator (never re-implement curve
   interpolation), then quantise as M9's format does -- 4x16-bit quats,
   per-track scale, keyframe reduction.
3. **Controller export** from an `AnimatorController` asset: states,
   transitions, conditions and parameters -> CTRL, name-hashed with the same
   FNV-1a the shim's `Animator` uses. Unsupported graph features (sub-state
   machines, blend trees beyond 1D, layers beyond the first) are reported by
   the validator with what will happen, never silently flattened.
4. **Skinned mesh export** -> SKMS: bone weights, and the per-batch bone
   palette that `vu_skin` needs (<=24 bones per batch, plan 7.3).
5. **Wire into the scene walk.** `PendingSkin` goes away; a
   `SkinnedMeshRenderer` in the scene exports because it is in the scene.
6. **`Animator` as a SCEN component** so `GetComponent<Animator>()` resolves
   -- today the animator is found only through the skinned renderer, which
   is the same indirection that made `Rigidbody` unreachable.

**Acceptance:** an imported (or procedurally built and *asset-serialised*)
character with a real `AnimatorController` walks, is told to run by a script
calling `SetFloat`, crossfades, and the on-target sample asserts the state
actually changed and the pose moved. Golden POSE-MATRIX parity against
Unity's sampler is retained.

## Task 2 -- `AudioSource`, `AudioListener`, `AudioClip`

M10's mixer is done: 24 voices, 3D pan and attenuation, streamed music,
voice stealing by priority.

1. `AudioSource` SCEN component: clip index, volume, pitch, loop,
   `playOnAwake`, `spatialBlend`, min/max distance.
2. `AudioClip` assets referenced by a source export into SND; resident SFX
   vs streamed music chosen by length and by the profile's budget.
3. Shim `AudioSource` (`Play`, `Stop`, `PlayOneShot`, `isPlaying`, `volume`,
   `pitch`, `loop`, `clip`) and `AudioListener` -- exactly one, and the
   validator errors on a second, as Unity warns.
4. The listener's transform drives `audio::set_listener` each frame; a 3D
   source's position drives its pan and attenuation.

**Acceptance:** a sample with a looping 3D source on a moving object and a
one-shot fired from a script; on-target assertion that the pan crosses zero
as the object passes the listener, and that `music_underruns()` stays 0.

## Task 3 -- `PlayerPrefs`

`runtime/include/ps2ur/memcard.h` already has `set_int`/`set_float`/
`set_string`/`has_key`/`delete_key`/`clear`/`serialize`/`deserialize`.

1. Shim `PlayerPrefs` over it, with Unity's semantics including the ones
   people rely on: `GetInt(key, defaultValue)`, `Save()` writing through to
   the card, and values surviving only after `Save()`.
2. Deviation: a memory card may be absent, full or write-protected.
   Unity's `PlayerPrefs` cannot fail; this one can. `Save()` returns void to
   match Unity, so failure is reported through `PS2Memory.LastSaveError`
   rather than silently swallowed, and the deviation is documented.

**Acceptance:** a sample writes prefs, saves, reloads the scene, reads them
back, and asserts the round trip on target against the emulator's virtual
card.

## Task 4 -- `PS2ParticleSystem`

Plan 7.2 replaces Unity's Particle System with a constrained one. Nothing
exists yet.

1. Runtime: a fixed-capacity pool per system, CPU simulation (position,
   velocity, gravity, lifetime, colour-over-life, size-over-life), drawn as
   camera-facing quads through the `Sprite2D`/`Additive` material kinds.
   Budgeted, not unbounded: a capacity the emitter cannot exceed.
2. Editor: a `PS2ParticleSystem` component with the subset that maps
   (emission rate, burst, shape sphere/cone/box, lifetime, speed, size,
   colour ramp, gravity modifier, looping, max particles).
3. The validator errors on Unity's own `ParticleSystem` with a message
   naming this as the replacement, rather than the generic unsupported text.
4. Shim class with `Play`/`Stop`/`Emit(count)`/`isPlaying`/`particleCount`.

**Acceptance:** a sample sustaining the profile's particle budget at 29.97
fps with a golden image, and a script-driven burst asserted on target.

## Task 5 -- uGUI subset

The largest task; the M8 overlay (`gs_overlay`) gives screen-space text and
rects as a substrate, and `tools/gen_font.py` already bakes bitmap fonts.
Everything else is new.

1. `RectTransform` anchoring/pivot/offset resolution, and a `Canvas` in
   screen-space-overlay only.
2. `Image` (sprite, colour tint, simple/sliced) and `RawImage` over the
   texture pipeline; `Text` over baked bitmap fonts with alignment and
   wrapping.
3. `Button` and `Slider` driven by the pad: a navigation graph rather than a
   pointer, because a DualShock 2 has no cursor. `EventSystem` is replaced
   by a `PS2UINavigation` that the docs describe honestly.
4. Draw the whole canvas in one pass after the 3D frame, sorted by hierarchy
   order, sharing the overlay's texture atlas.

**Acceptance:** a menu sample -- title, two buttons, a volume slider --
navigated with the D-pad and confirmed with X, asserted on target by tokens
for focus movement and activation, plus a golden image per screen.

## Task 6 -- Close the loop

1. Move each completed component from `DropReasons` to `ExportedComponents`
   in `PS2ContentValidator`, which is the mechanical check that a task is
   done.
2. Update `docs/supported-api.md`: every new deviation numbered, and the
   7.1 table marked component by component.
3. One sample that uses all of it at once -- the D1 candidate: a character
   animating, sound playing, particles emitting, a menu over the top.
4. `docs/notes/verify-log.md` entries for every trap, and the CLAUDE.md
   status block.

---

## Order and why

Tasks 1-3 first: they are wiring over runtimes that already pass their
tests, so they convert finished-but-unreachable work into usable features
at the lowest cost per component. Task 4 next: self-contained, and the
particle draw path exercises the `Sprite2D` material kind that Task 5's UI
also needs. Task 5 last: it is the biggest, it depends on the sprite path,
and it is the one most likely to want its own milestone.
