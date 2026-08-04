# Supported API

Seeded from `ps2port.txt` section 7 (the scope contract). This contract is
enforced by an Editor-side validator (TODO(spec missing: section 13.5)) that
fails the build with actionable errors rather than producing a broken ISO.

NOTE: plan section 7.2 is missing (the plan text jumps from 7.1 to 7.3). It is
presumed to be the explicit "not supported" list. TODO(spec missing: section
7.2): author the unsupported/forbidden list.

## Supported (plan section 7.1)

| Area | Supported |
|---|---|
| Language | Full C# 9-ish as IL2CPP supports it: classes, structs, generics, interfaces, delegates, events, `foreach`, LINQ (with caveats), `async`/`await` over a cooperative scheduler |
| Core types | `Vector2/3/4`, `Quaternion`, `Matrix4x4`, `Color`, `Color32`, `Rect`, `Bounds`, `Mathf`, `Random` |
| Object model | `Object`, `GameObject`, `Component`, `Transform`, `MonoBehaviour`, `ScriptableObject` (assets only), tags, layers, `Find`/`GetComponent` family |
| Lifecycle | `Awake`, `OnEnable`, `Start`, `Update`, `FixedUpdate`, `LateUpdate`, `OnDisable`, `OnDestroy`, coroutines (`WaitForSeconds`, `WaitForFixedUpdate`, `WaitUntil`, `null`) |
| Rendering | `MeshFilter`, `MeshRenderer`, `SkinnedMeshRenderer`, `Camera` (perspective + ortho), a fixed material model (below), `Light` (directional + ambient, baked-ish), sorting layers |
| Animation | `Animation`-style clip playback with crossfade, additive blending, root motion; a simplified `Animator` supporting states + transitions authored in a restricted controller |
| Physics | `Physics.Raycast`/`SphereCast`/`OverlapSphereNonAlloc` against a baked BVH and primitives; `Rigidbody` (semi-implicit Euler); `BoxCollider`, `SphereCollider`, `CapsuleCollider`, `MeshCollider` (static, baked to world space offline); `CharacterController` (swept capsule, step + slope); trigger + collision callbacks (deviations 16-21) |
| Audio | `AudioSource` (2D + simple 3D pan/attenuation), `AudioClip` (streamed music, resident SFX), `AudioListener` |
| Input | `Input.GetAxis`/`GetAxisRaw`/`GetButton*` over Unity's default axis and button names, mapped to DualShock 2; `PS2Input` for per-button access, analog pressure, rumble and port 2 (deviations 11-12) |
| UI | An immediate-mode-backed subset of uGUI: `Canvas` (screen space overlay), `Image`, `RawImage`, `Text` (bitmap fonts baked offline), `Button`, `Slider` |
| Persistence | `PlayerPrefs`-equivalent on memory card, plus a save API with icon support |
| Scene management | `SceneManager.LoadScene`/`LoadSceneAsync` by name, `AsyncOperation` (yieldable from a coroutine, with `progress` and `allowSceneActivation`), additive loading (deviations 13-14) |
| Resources | An `Addressables`-like async load from a build-ordered disc layout |

## Material model (plan section 7.3)

There are no shaders. There is a fixed set of material *kinds*, each mapping to
a VU1 microprogram + GS register configuration:

| Kind | VU1 program | Notes |
|---|---|---|
| `Unlit` | `vu_unlit` | Texture x vertex colour |
| `VertexLit` | `vu_lit` | **Three** directional lights + ambient, per vertex. The plan says four; the microprogram's MADD chain spends its fourth accumulate slot on ambient, so three is what the hardware path actually delivers (verify-log M9). |
| `VertexLitFog` | `vu_lit_fog` | `VertexLit` plus per-vertex fog factor (GS `FOGCOL` + `PRIM.FGE`) |
| `LitAlpha` | `vu_lit` + alpha blend state | Sorted back-to-front, no Z write |
| `Cutout` | `vu_lit` + alpha test | Z write on |
| `Additive` | `vu_unlit` + additive blend | Effects |
| `Skinned` | `vu_skin` | Matrix palette in VU1 data mem; <=24 bones per batch |
| `Sprite2D` | `vu_sprite` | Screen-space, no transform |

Authoring: a `PS2Material` asset, plus an importer that maps common
Standard/Mobile shaders onto the closest kind and warns on loss.

## Documented conformance deviations (plan section 7.4)

These are listed prominently here and asserted in the conformance suite
(`managed/PS2.Conformance`) so they never regress silently:

1. `float` arithmetic is not IEEE 754. NaN and Infinity do not survive hardware
   float ops; overflow saturates. `double` is IEEE-correct but
   software-emulated and slow.
2. `double` arithmetic is 20-100x slower than `float`. The validator warns on
   `double` in hot paths.
3. String formatting/parsing is culture-invariant only; no ICU, no localisation
   tables.
4. `DateTime` resolution is limited by the RTC; time zones are UTC-only.
5. Deterministic GC pauses are not guaranteed; the profiler exposes them.
6. Skinning takes at most 4 influences per vertex and 24 bones per batch, and
   transforms normals by the blended matrix rather than its inverse
   transpose: bones with NON-UNIFORM scale light incorrectly (uniform scale,
   what a character rig uses, is exact).
7. `Animator.CrossFade` (normalized transition duration) is absent;
   `CrossFadeInFixedTime` (seconds) is provided and exact. A member that
   looked like Unity's but measured time differently would be worse than no
   member at all.
8. **Audio has 24 voices and no preemption.** On the audsrv mixing model a
   sounding voice cannot be stopped early, so `AudioSource.Stop` releases the
   handle but the sample plays out, and a sound started while all 24 voices
   are busy is dropped regardless of priority. Priority still decides which
   voice *would* be sacrificed, and flips to real stealing when the custom
   IRX lands (plan section 9, M10 task 1).
9. Per-source audio **pitch** is not supported: audsrv fixes pitch on the
   loaded sample, not the playback. A clip needed at several pitches must be
   exported several times.
10. SFX are downmixed to **mono** at export (22.05 kHz by default). Stereo
    content belongs on the streamed music path, which is stereo.
11. **`Input.GetKey` and the Input Manager's configurable axes are absent.**
    There is no keyboard to key off and no inspector in which to configure an
    axis, so `Input` exposes `GetAxis`/`GetAxisRaw` and `GetButton*` over
    Unity's default axis and button names only (`Horizontal`, `Vertical`,
    `Mouse X`/`Y` -> the right stick, `Fire1`-`Fire3`, `Jump`, `Submit`,
    `Cancel`). An unknown name returns 0/false, exactly as Unity does for an
    unconfigured axis. Everything the DualShock 2 has and Unity cannot
    describe -- per-button access, analog pressure, rumble, port 2 -- lives on
    `PS2Input`.
12. **`GetAxisRaw` and `GetAxis` return the same value.** Unity's `GetAxis`
    applies an Input Manager smoothing filter; with no Input Manager there is
    no filter to apply, and inventing one would make `GetAxisRaw` a lie.
    Smooth in the game if you want smoothing.
13. **`SceneManager` addresses scenes by name only, and there is no
    `UnloadSceneAsync`.** There is no Build Settings scene list to index into
    on a disc, so the build-index overloads are absent; a name maps to
    `<name>.p2b`. Unloading is absent because an additively loaded scene's
    meshes point straight into the container it was read from (zero copy), so
    a single scene cannot be pulled back out of a merged world. Load
    `LoadSceneMode.Single` to get back to one scene.
14. **A scene's camera and light do not come across in an additive load.**
    Everything else does -- entities, meshes, materials, scripts, skeletons,
    clips, controllers and skinned characters, all with their indices
    rebased -- but the running scene keeps the camera the player is looking
    through. An additive load that would overflow any table is refused whole,
    never applied halfway.
15. Reflection is limited to what survives managed stripping; `link.xml` is
    mandatory for any reflective code.
16. **Physics is not PhysX and does not try to be** (ADR-009, and the plan
    says so in as many words). Contact behaviour, resting jitter and
    stacking all differ from the Editor. What matches exactly is the API
    shape, so gameplay code compiles and reads the same. Deviations 17-21
    are the specifics.
17. **One contact point per pair, one position-correction pass, no
    constraint solver.** No manifolds, no joints, no friction model (a fixed
    tangential damping stands in for it). A resting box settles but may
    creep; a tower of three boxes will not stay a tower. Gameplay that needs
    reliable stacking should use `CharacterController` and kinematic bodies,
    which is what PS2-era games did. Measured cost of what is there: 71 us
    average, 154 us worst, against a 4 ms budget.
18. **Colliders are baked, not constructed at runtime.** Static mesh
    collision is baked to world space offline, which is what makes its
    broadphase free -- so `AddComponent<BoxCollider>()` mid-game is not
    supported, a `MeshCollider` cannot move, and a convex `MeshCollider`
    (Unity's hull-for-a-dynamic-body) is refused by the exporter rather than
    silently reinterpreted as static triangles.
19. **`RaycastHit` names a collider INDEX, not a `Collider` component**, and
    reports -1 for a hit on baked static geometry -- which has no GameObject
    in this runtime. `RaycastAll`, `CapsuleCast`, `OverlapBox`/`Capsule` and
    `ComputePenetration` are absent; `OverlapSphereNonAlloc` is provided
    because a console has no business allocating an array per query.
20. **The `Collision` object passed to callbacks is REUSED, not allocated
    per event.** A 4 MB managed heap with a 2 ms GC budget cannot afford one
    allocation per contact per frame. Do not cache it -- copy what you need.
    `OnTrigger*` receives a `Collision` too, not a `Collider`, since a
    Collider here is an index rather than a component.
21. **Physics is deterministic per build, not across builds.** The step is
    fixed, iteration order is array order, and there is no `double` -- so
    the same inputs give the same result every run of the same binary. The
    EE's floats are not IEEE 754 (deviation 1), so a compiler change can
    move the last bit. Replays shared between differently-built binaries,
    and lockstep multiplayer, cannot rely on it.
22. `Rigidbody` omits `constraints` beyond `freezeRotation`,
    `interpolation`, `collisionDetectionMode`, `centerOfMass`,
    `inertiaTensor`, `AddTorque`, `AddExplosionForce` and `AddForce` modes
    other than `Force`. Each needs a solver this runtime does not have, and
    a property that read back what you set while changing nothing would be
    worse than a compile error.
23. **A `Rigidbody` needs a `Collider` on the same GameObject to move at
    all.** The solver addresses transforms by collider index -- `phys::step`
    takes one position per collider -- so a body with no collider integrates
    a velocity into a slot that does not exist. In Unity such a body still
    falls. Here it cannot, so `Rigidbody.Bind` logs an error naming the
    object, and the build validator reports it before the build runs.
24. **Components carried into the build are a smaller set than components
    the runtime supports.** Exported from the scene: `MeshFilter`/
    `MeshRenderer`, `Camera`, a Directional `Light`, `Rigidbody`, the four
    collider types, and (M12.5) `SkinnedMeshRenderer`, `Animator` -- both
    skinned and rigid-bound models, the latter binding bones to entities BY
    NAME, so renaming a bone after export breaks it -- plus `AudioSource`
    and `AudioListener`. `Animation` (legacy) and an Editor-placed
    `CharacterController` are still not carried. The build validator reports
    each dropped component with what happens on target, because "supported"
    and "exported" being different sets is exactly how a `GetComponent`
    came to return null on the console while every individual piece looked
    present.
25. **Only Directional lights are exported.** Vertex lighting on VU1 takes a
    direction and a colour; a point or spot light has no equivalent, so an
    object lit only by one renders unlit.
26. **`AudioSource.loop` cannot change at runtime.** SPU2 ADPCM loop points
    are baked into the sample stream when the clip is encoded at export, so
    looping is a property of the CLIP on this hardware. The exporter encodes
    a clip looped when the source that references it loops (one asset used
    both ways is encoded twice). Assigning a different value to `loop` at
    runtime logs an error naming this deviation instead of silently playing
    the other behaviour. `pitch` is absent for the same class of reason:
    audsrv offers no per-voice repitch.
27. **`AudioSource.isPlaying` tracks the voice `Play()` started.** Unity
    counts live one-shots too; here `PlayOneShot` is fire-and-forget on the
    mixer and never tracked, so `isPlaying` can read false while a one-shot
    still sounds. Note also that a sound can be DROPPED outright when all 24
    voices are busy with higher-priority audio -- `Play()` then leaves
    `isPlaying` false, which is the platform behaving as designed (M10), not
    an error.
28. **The PlayerPrefs store is finite.** A memory card save is a fixed
    file: 64 keys, 24-byte keys, 48-byte string values (memcard.h). A Set
    beyond a limit logs an error naming the limit and does not store, where
    Unity's version is unbounded. Budget prefs like save data, because that
    is what they are.
29. **`PlayerPrefs.Save()` can fail.** A card can be absent, unformatted,
    full or write-protected -- states Unity's API has no words for. Save()
    stays void to match Unity; the result lands in
    `PS2Memory.LastSaveError` (`PS2SaveStatus`), a warning is logged on
    anything but Ok, and values survive a power cycle only after a Save
    that reported Ok. There is no auto-save on quit: a PS2 game has no
    quit.
30. **uGUI layout is baked at export.** RectTransform anchors, pivots and
    offsets are resolved against the profile's framebuffer (the CanvasScaler
    is ignored: the reference resolution IS the framebuffer), and the runtime
    holds finished screen rects. Scripts move elements in pixels; anchors do
    not exist at runtime. Text uses the scene's REAL fonts: every
    (font, fontSize) pair a Text component uses is rasterised by Unity's
    own font engine at export and baked into an atlas (up to four pairs
    per scene; overflow and bake failures fall back to a builtin 8x8
    font, loudly). Glyphs draw proportionally, antialiased, at 1:1 baked
    pixels -- what the Editor shows at that size is what the console
    draws. Consequences: fontSize is a BAKE-TIME property (a script
    changing it at runtime keeps the baked glyphs), glyph coverage is
    printable ASCII, and the 48-byte cap per element stands. Image supports
    Simple (stretch) and Sliced (real 9-slice from sprite.border, baked at
    export); Tiled draws as Sliced; Filled is not supported. Sprite-atlas
    SUB-RECTS are not supported -- the sprite's whole texture draws, so give
    UI sprites standalone textures (the exporter warns).
31. **`PS2UINavigation` replaces `EventSystem`.** A DualShock 2 has no
    pointer, so focus moves with the D-pad in hierarchy order, Cross submits
    the focused Button, Left/Right step the focused Slider by a tenth of its
    range. A focused control tints its Graphic toward a warm gold and
    restores the authored colour on unfocus -- Unity's ColorBlock states
    are not modelled, and the tint is deliberately stronger than Unity's
    near-invisible default highlight, because a D-pad menu on a TV lives
    or dies by knowing where the cursor is. Exported Buttons and Sliders
    register automatically; EventSystem
    and its input modules export as nothing and are reported as such.
    `onClick`/`onValueChanged` take listeners IN CODE -- Inspector-serialised
    persistent listeners do not exist on this platform.
32. **Scene transitions double-buffer the asset pool.** A game that uses
    `SceneManager.LoadScene`/`LoadSceneAsync` holds TWO asset-pool arenas:
    the incoming scene streams into one while the outgoing scene -- whose
    meshes live zero-copy in the other -- is still drawn. Budget
    accordingly: transitions cost one extra Asset Pool of RAM, and a game
    that cannot fit both boots with transitions disabled and a message
    saying so. On a Single load every non-persistent object is destroyed
    (OnDisable/OnDestroy fire) and the new scene's components and scripts
    are created exactly as at boot; PlayerPrefs survives. ADDITIVE loads
    append but occupy the second arena permanently: after one additive
    load, further loads are refused until a Single load frees an arena,
    and an additive scene's PHYS section is ignored (static collision is
    baked per scene and cannot merge). Scripts carry no serialised
    fields, so scene names passed to LoadScene belong in code, not the
    Inspector.

33. **Cutout materials with a texture render UNLIT.** The runtime has no
    lit-and-textured vertex layout, so a `TransparentCutout` material
    with a main texture exports as the textured layout plus a GS alpha
    test on the sampled alpha: leaf and fence shapes cut correctly, but
    the surface ignores scene lighting (bake lighting into the texture
    or vertex colours, the PS2-era norm). An untextured cutout keeps
    vertex lighting, as before. Large triangles are subdivided at export
    (max edge ~6 world units) because the VU1 pipeline REJECTS
    near-plane-crossing triangles rather than clipping them; meshes over
    ~1,600 triangles after subdivision are refused at load and warned
    about at export.

34. **CharacterController configures before first use.** The native
    capsule is created on the first `Move()`/`isGrounded`, taking the
    radius/height/center/stepOffset set since `AddComponent` with
    Unity's transform scaling applied. Dimensions changed AFTER that
    first use do not reach the native capsule (Unity re-shapes it
    live); set them up front. `center` is supported and necessary for
    feet-origin characters. MeshColliders bake to static world-space
    triangles at export and need Read/Write enabled on the source
    model; convex MeshColliders are refused (baked collision is
    static).
