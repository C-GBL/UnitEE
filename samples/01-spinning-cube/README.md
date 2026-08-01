# 01-spinning-cube

One textured cube rotating via a `MonoBehaviour` `Update` -- the first sample
where user C# executes on target.

Purpose (plan section 8): exercises mesh export (tri-stripping, VU1 batching),
texture export, per-frame transform updates from managed code, and the IL2CPP
script path end to end.

This sample is the designated early test case for the CLUT quirk from plan
section 3.3: its texture is authored so that a missing CSM1 32-entry palette
block swap (blocks 1-2 and 5-6 of each group of 8) is immediately visible as
correct shapes with scrambled colours.

Acceptance links:
- D5 (plan section 2): first conformance check that a script's behaviour
  (rotation over `Time.deltaTime`) matches Editor play mode.
- D7 (plan section 2): golden-image coverage for textured, lit geometry.

Status: placeholder. Blocked on the exporter, the IL2CPP port, and the shim.
TODO(spec missing: section 9).
