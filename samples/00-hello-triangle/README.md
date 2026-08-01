# 00-hello-triangle

The M0 deliverable (plan section 9, M0 task 5): the smallest program that proves
the whole toolchain works end to end.

It initialises the GS through ps2sdk `libgraph`/`libdraw` at the plan's baseline
video mode (512x448, PSMCT32 -- section 3.3), clears the screen to a known
colour, draws one Gouraud-shaded triangle via a raw PACKED GIF packet, and
prints a token to the EE console.

## Why it depends only on ps2sdk

This sample deliberately does **not** link `ps2ur`. It is the canary that tells
you whether the toolchain broke or your runtime change broke, so it has to keep
booting while the runtime is being torn apart.

That also means it is not yet a *Unity* sample. The Unity-authored version of
this scene -- profile > Build > `scene.p2b` > ELF, exercising D1 and D7 -- needs
the exporter and container reader, which arrive at M5. This directory will gain
that project then; the C sample stays as the toolchain canary.

## Build and run

```sh
cmake --preset ps2-release && cmake --build --preset ps2-release
./tools/ci/run-emu-test.sh \
    build/ps2-release/samples/00-hello-triangle/00-hello-triangle.elf \
    PS2UR_TOKEN_HELLO_TRIANGLE_OK
```

The harness boots PCSX2 headless and asserts the token appears in the emulator
log (M0 task 6). Two things it handles for you, both of which otherwise produce
failures that look like guest crashes: PCSX2 needs an **absolute** ELF path, and
it rejects paths containing a **space** -- so the ELF is staged to a space-free
directory first. See `docs/notes/verify-log.md`.

EE `printf` only reaches the log when `EnableEEConsole = true` in PCSX2's ini.
An empty log is almost always that, not a dead program.

## Acceptance

- Token `PS2UR_TOKEN_HELLO_TRIANGLE_OK` present in the emulator log. Verified
  2026-07-31, found 2 s after boot.
- Visually: a blue-grey field (32, 96, 160) with a red/green/blue triangle.

## Note on the name

The plan's M0 task only asks for a screen clear; the triangle is included
because the sample is named for one and because a single GIF primitive is the
natural first step toward M2's `DrawTrianglesImmediate`. Textures, real
geometry, and VU1 microprograms arrive at M2 and M4.
