# ADR-010: A component-coverage phase (M12.5) runs before M13

Status: accepted (2026-08-02)
Context: plan section 7.1 (the scope contract), section 9 M9/M10/M12/M13.
Related: ADR-007 (subset discipline), ADR-005 (Editor integration).

## Context

M12 delivered the button. A Unity scene becomes a bootable ELF with no
manual steps, and the acceptance case -- the user's own scene, running on
target -- passes.

Immediately after it passed, the user's script threw a
`NullReferenceException` on the frame they pressed a button.
`GetComponent<Rigidbody>()` had returned null. The component was in the
validator's supported list, the shim implemented it, the native solver
simulated it, the bridge exposed it, and the scene exporter carried no
record of it. Every individual piece was present and tested; the call
between two of them was missing, and no unit test can see a call that
nobody wrote.

Fixing that (commit `ad10b05`) forced an audit of the rest of section 7.1,
and the same hole is there four more times:

| Promised in 7.1 | Runtime | Shim class | Export path |
|---|---|---|---|
| `SkinnedMeshRenderer`, `Animator` | complete (M9) | complete | **none** |
| `AudioSource`, `AudioListener` | complete (M10) | **none** | **none** |
| `PlayerPrefs`-equivalent | complete (M10) | **none** | n/a |
| uGUI subset | overlay text/rects only (M8) | **none** | **none** |
| `PS2ParticleSystem` (7.2) | **none** | **none** | **none** |

The animation case is the sharpest. M9's acceptance was real: golden
POSE-MATRIX parity against Unity's own sampler, max error 0.019 over a
five-second clip, `vu_skin` matrix-palette skinning, crossfades at 29.97 fps.
All of it runs. And the only way to get a controller into a `.p2b` is
`PS2SkinExportMenu`, which builds a procedural 24-bone test rig in code.
A user with an imported character, an `AnimatorController` asset and a
`SkinnedMeshRenderer` has no path at all. The runtime is finished and
unreachable.

## Decision

Insert **M12.5 -- component coverage** between M12 and M13, defined in
`docs/m12-5-component-coverage.md`.

Its job is to make section 7.1 true. It adds no scope: every item is
already promised by the scope contract, and most of the work is connecting
an Editor export path to a runtime that already exists and already passes
its tests.

`ps2port.txt` is not modified. It is the authoritative plan and section 18.1
forbids editing it. This is a sequencing decision about work the plan
already specifies, which is exactly what an ADR is for.

## Why before M13 and not after

M13 is "performance, memory, and real hardware". Three reasons it should
not come first.

**Profiling an incomplete engine measures the wrong thing.** M13 sets
budgets against the frame the game actually draws. A frame with no UI, no
particles and no animated characters is not that frame. Tuning it and then
adding three subsystems means doing M13 twice.

**The hardware half of M13 is blocked anyway.** The user's console died
mid-project. Plan 14.4 already reduces PCSX2 to gating relative change,
and 14.5's leniency checklist is explicitly the on-hardware starting point.
Waiting on a replacement console while the component gap stays open would
idle the part of the project that is not blocked.

**The gap is what a user hits first.** Nobody meets the frame budget before
they meet `GetComponent<Animator>()` returning null. D1 (install, open the
sample, press Build and Run, play) is the project's headline acceptance
criterion, and a sample that cannot animate a character, play a sound or
draw a menu does not demonstrate it.

## Consequences

M13 and M14 shift later. The plan's own numbering is untouched; M12.5 is a
decimal deliberately, so a reader comparing this repository against
`ps2port.txt` sees at once that it is an insertion and not a renumbering.

The phase is large -- uGUI alone is comparable to a full milestone -- so it
is split into six tasks that land and are verified independently, in the
order in `docs/m12-5-component-coverage.md`. The first three are wiring
work over finished runtimes; the last two are new subsystems.

## What this phase must not become

A second chance to widen the API surface. ADR-007's subset discipline still
governs: a member exists only if it behaves as Unity's does. The failure
this phase exists to fix is a component that is *absent* while looking
present, and the cure for that is not a component that is *present* while
doing nothing. Each task's acceptance is an on-target assertion that the
component does its job, not that it constructs.

Two guards from M12 apply to every task here, and they are why this phase
can be trusted where M9 and M10 could not:

- The build fails when its output directory is incomplete, even if every
  step reported success.
- The content validator separates "the runtime supports it" from "the
  exporter carries it", and reports the difference. Each task in this phase
  moves a component from the second list to the first, and that move is
  itself the check that the task is done.
