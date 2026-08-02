# ADR-008: M10 platform services -- input, scene loading and disc layout

Status: accepted (2026-08-02)
Context: plan section 9, M10 tasks 2, 4 and 5. Supersedes nothing.
Related: ADR-002 (managed/native boundary), ADR-007 (subset discipline).

## Context

M10 brings up the console's platform services: audio, pads, memory card,
streaming I/O and scene loading. Three of them force a design decision
because Unity's own API either does not fit the hardware or does not exist
for it. This ADR records those three; audio's no-preemption constraint and
the memory-card model are recorded as conformance deviations 8-10 in
`docs/supported-api.md` and in the verify-log, and needed no architectural
choice beyond "report what the hardware can actually do".

## Decision 1: `Input` is Unity-shaped over default names only; `PS2Input`
carries the rest

Unity's `Input` is built around the Input Manager: a project asset that maps
named axes to devices. A PS2 build has no inspector in which to author one,
and shipping a runtime parser for `InputManager.asset` would buy a
configurability nobody can exercise on the target.

So `UnityEngine.Input` supports Unity's DEFAULT names and nothing else --
`Horizontal`, `Vertical`, `Mouse X`/`Y`, `Fire1`-`Fire3`, `Jump`, `Submit`,
`Cancel`, `Start` -- mapped to the DualShock 2. An unknown name returns
0/false, which is what Unity does for an unconfigured axis, so the failure
mode matches. `GetKey` is absent entirely: there is no keyboard, and a
`GetKey` that always returned false would be a member that exists and does
nothing (ADR-007).

`GetAxis` and `GetAxisRaw` return the SAME value. Unity's `GetAxis` applies
the Input Manager's smoothing filter; with no Input Manager there is no
filter, and synthesising one would make `GetAxisRaw` -- whose whole contract
is "unfiltered" -- a lie in the other direction.

Everything the pad has that Unity has no vocabulary for goes on `PS2Input`,
which makes no pretence of being Unity: per-`PS2Button` access, analog
pressure 0..255, rumble, and the second port. A game that wants those is
already writing PS2-specific code and is better served by an API that says
so.

### Consequence

Porting a project that uses custom axis names is a compile-clean, run-time
silent no-op (the axis reads 0). The Editor-side validator (plan 13.5) must
flag `GetAxis`/`GetButton` calls with names outside the supported set. That
is tracked as an open item, not solved here.

## Decision 2: scene loading is driven from managed, one load at a time,
with the buffer owned by the host

`SceneManager.LoadSceneAsync` returns an `AsyncOperation` that a coroutine
can `yield return`, matching the Editor. Three sub-decisions:

**The frame loop pumps the load, not the coroutine.** `Runtime.Tick` calls
`SceneManager.Pump()` at the top of every frame, which advances the native
loader by a bounded 64 KB. A load therefore progresses whether or not
anything is waiting on it, and `yield return op` needs no special scheduling
-- the coroutine pump simply keeps waiting while `!op.isDone`.

**One load in flight.** The loader writes into a single host-owned buffer and,
on completion, mutates the live world; two concurrent loads would interleave
both. Starting a second while one runs is refused with an error, not queued.

**The host owns the destination buffer** (`bridge::bind_scene_buffer`).
Managed code cannot sensibly decide where 4 MB of EE RAM comes from, and the
world points straight into that buffer (zero copy), so its lifetime is a
host-program concern. Unbound, `LoadSceneAsync` fails loudly rather than
reading into nothing.

`allowSceneActivation` is implemented honestly rather than approximated: the
loader's existing `Parsing` state becomes an observable park at progress 0.9
with the world untouched, and granting activation lets the next update finish
the swap. That is exactly what Unity's flag means, and it exists so a game
can complete a fade before the scene changes underneath it.

### Consequence

`UnloadSceneAsync` cannot be provided (deviation 13): an additively merged
scene's meshes point into the container it was read from, so it cannot be
extracted again. `LoadSceneMode.Single` is the way back to one scene.

## Decision 3: disc order comes from a recorded trace, not from a manifest

The runtime records first-access order for every file it opens
(`stream::trace_entry`), and `tools/disc/layout_planner.py` turns a profiling
run's log into the mkps2iso script whose `<file>` order IS the LBA order.

The alternative -- letting the build declare an intended order -- was
rejected because the declaration would be a guess that silently rots as the
game changes, and nothing would ever check it. A trace is measured, and it
is measured from the same log the on-target tests already produce, so
profiling costs nothing extra. The planner reports the seek cost of the
planned order against the unplanned one so the benefit is a number rather
than an assertion.

### Consequence

The trace has a fixed capacity (`stream::kMaxTraceEntries`, 64) and records
first touch only. A game with more than 64 distinct files needs that raised
before its layout can be planned; the planner reports files it saw in the
trace but not in the staging directory, which is what catches a stale trace.
