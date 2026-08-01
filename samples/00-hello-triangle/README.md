# 00-hello-triangle

The smallest possible end-to-end build: one scene, one triangle, a fixed camera,
no textures, no scripts beyond scene boot.

Purpose (plan section 8): first bring-up sample for the pipeline. It exercises
the minimum path -- profile > Build > scene.p2b > ELF > boots in PCSX2 -- using
the EE-only software-transform bootstrap renderer (ADR-003 retains the PATH3
path for bring-up before VU1 microprograms exist).

Acceptance links:
- D1 (plan section 2): the one-click build check runs against this sample first,
  since it is the cheapest project that must produce a bootable image.
- D7 (plan section 2): first golden-image target; a single flat-shaded triangle
  gives a trivially stable framebuffer CRC.

Status: placeholder. The Unity sample project cannot exist until the exporter
and runtime bring-up land. TODO(spec missing: section 9) -- the milestone plan
that schedules this sample is not yet available.
