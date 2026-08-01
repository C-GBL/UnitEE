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
| Physics | Raycast/spherecast against static geometry; `Rigidbody` with a simple integrator; `BoxCollider`, `SphereCollider`, `CapsuleCollider`, `MeshCollider` (static, convex-decomposed offline); `CharacterController`; trigger + collision callbacks |
| Audio | `AudioSource` (2D + simple 3D pan/attenuation), `AudioClip` (streamed music, resident SFX), `AudioListener` |
| Input | `Input.GetKey`/`GetAxis` mapped to DualShock 2, analog sticks, pressure buttons, rumble |
| UI | An immediate-mode-backed subset of uGUI: `Canvas` (screen space overlay), `Image`, `RawImage`, `Text` (bitmap fonts baked offline), `Button`, `Slider` |
| Persistence | `PlayerPrefs`-equivalent on memory card, plus a save API with icon support |
| Scene management | `SceneManager.LoadScene` (sync + async with a loading screen), additive loading |
| Resources | An `Addressables`-like async load from a build-ordered disc layout |

## Material model (plan section 7.3)

There are no shaders. There is a fixed set of material *kinds*, each mapping to
a VU1 microprogram + GS register configuration:

| Kind | VU1 program | Notes |
|---|---|---|
| `Unlit` | `vu_unlit` | Texture x vertex colour |
| `VertexLit` | `vu_lit` | Up to 4 directional lights + ambient, computed per vertex |
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
6. Reflection is limited to what survives managed stripping; `link.xml` is
   mandatory for any reflective code.
