# ADR-007: Editor play mode uses Model A (strict subset, no shim in Editor)

Status: accepted, 2026-08-01 (M7 task 7).

## Context

Users write MonoBehaviours in the Unity Editor and expect play mode to work.
The PS2 build compiles those same sources against `PS2.UnityShim`. Two
models existed (plan section 9 M7 task 7):

- **Model A**: the shim's public API is a strict subset of Unity's real API.
  In the Editor, user code runs against real Unity; the shim exists only for
  the PS2 compile.
- **Model B**: users reference `PS2.UnityShim` explicitly everywhere,
  including in the Editor.

## Decision

Model A, as the plan recommends. The enforcement mechanism is the PS2
recompile itself (plan section 12.3): user sources are rebuilt against the
shim with the same defines plus `UNITY_PS2`, and any API outside the subset
fails THAT compile with a normal C# error pointing at the user's line. The
M7 acceptance ran exactly this path: `Spin.cs` and `SpinParityCheck.cs`
written against real Unity in the Editor, recompiled untouched against the
shim, behaviour verified frame-accurate on target (max matrix deviation
5.7e-05 over 300 frames against an Editor-recorded golden).

Consequences of the subset rule (CLAUDE.md hard rule: a member that exists
but does not match Unity's semantics is worse than a compile error):

- Shim members are added ONLY with Unity-exact behaviour; missing members
  are the correct failure mode (compile error on the PS2 build).
- Unsupported-but-tempting members that exist for API-shape reasons throw
  `NotSupportedException` with an explanation (`Object.Instantiate`,
  delayed `Destroy`); nothing silently no-ops.
- `Debug.Log`'s optional `[CallerFilePath]/[CallerLineNumber]` parameters
  are the one signature widening: invisible at Unity-compatible call sites,
  additive, and semantically identical.

The `PS2.UnityShim.Editor` assembly remains a placeholder: Model A removes
the need for an Editor-side stand-in. The conformance suite
(`PS2.Conformance`) covers the pure-managed subset (math) on the host; the
native-backed object model is covered by `runtime/tests` on the host and
the M7 on-target parity test.
