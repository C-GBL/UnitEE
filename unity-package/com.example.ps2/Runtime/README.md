# Runtime (editor-play-mode stand-ins)

This assembly (`com.example.ps2.Runtime`) will hold the editor-play-mode
stand-in components described in the repository layout (plan section 8,
`unity-package/.../Runtime/ # editor-play-mode stand-ins`).

Purpose: when a user presses Play in the Unity Editor, their scripts run against
real Unity. On the PS2 they run against the `PS2.UnityShim` facade assembly and
the `ps2ur` native runtime. The stand-ins in this folder are the Editor-side
counterparts of PS2-only concepts (for example `PS2Material` kinds, plan section
7.3) so that play mode previews the constrained PS2 behaviour and the D5
conformance criterion (identical Editor/on-target behaviour, plan section 2) can
be tested inside the Editor.

Current state: `PS2RuntimeStandIn` is an empty placeholder. The real stand-ins
arrive with the managed shim integration. TODO(spec missing: section 12) -- the
shim specification that defines the stand-in surface is not yet available.
