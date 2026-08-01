# ADR-005: Editor integration surface

Status: Accepted (plan section 5, ADR-005)

## Context

The user-facing requirement is "adds a build profile that builds a PS2 game
straight from Unity". Unity does not permit registering new `BuildTarget`
enum values, so a first-class native platform entry in the Build Profiles
window is impossible.

## Options

- A real platform/BuildTarget registration: not an extension point Unity
  exposes; impossible without the partner path (see ADR-001 context).
- A custom asset + window that mirrors Unity 6 Build Profiles vocabulary,
  plus hooks into the standard build pipeline: available today with public
  Editor APIs.

## Decision

- A **`PS2BuildProfile : ScriptableObject`** asset that mirrors the shape and
  vocabulary of Unity 6 Build Profiles (scene list, scripting defines,
  per-profile settings), created via
  `Assets > Create > Build Profiles > PlayStation 2`.
- A **dedicated Editor window** (`Window > PS2 > Build Profiles`) styled
  after the built-in Build Profiles window, with **Build**, **Build and
  Run**, and **Clean** buttons.
- Hooks into the standard pipeline where useful: `BuildPlayerProcessor` and
  `IPreprocessBuildWithReport`, so a normal build of a stand-in desktop
  profile can optionally trigger a PS2 build alongside it (useful for CI).

## Consequences

- This satisfies the requirement within the Editor's actual extension
  points; the PS2 entry does not appear in the built-in Build Profiles
  window and docs must set that expectation.
- CI can drive builds headlessly (`-batchmode -executeMethod`, per D1) either
  via the window-backing pipeline API or via the stand-in-profile hook.
- The window/asset implementation lives in
  `unity-package/com.example.ps2/Editor/BuildProfile/`; details are governed
  by the missing Editor spec (TODO(spec missing: section 13)).
- Mirroring Build Profiles vocabulary is a deliberate UX choice; if Unity's
  Build Profile APIs shift in a future 6000.x, only our window is affected,
  not the pipeline.
