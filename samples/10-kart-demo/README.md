# 10-kart-demo

The D2 reference game (plan section 2): 3 scenes, a drivable vehicle, a skinned
character, HUD, music, SFX, and save/load to memory card.

Purpose (plan section 8): this is the project the definition of done is measured
against. It deliberately touches every supported subsystem from the plan section
7.1 contract -- rendering (including `SkinnedMeshRenderer`), physics
(`CharacterController`, raycasts, rigidbody integration), animation, audio
(streamed music + resident SFX), input (analog sticks, rumble), uGUI subset for
the HUD, persistence, and scene management.

Acceptance links (plan section 2):
- D2: runs at >= 30 fps sustained, verified by PCSX2 frame-time log and real
  hardware capture.
- Also the primary vehicle for D3 (real hardware boot from DVD-R and USB),
  D4 (peak RAM <= 30 MB over a 30-minute soak), and D6 (clean build <= 15
  minutes, incremental script change <= 3 minutes).

Budget targets while authoring: stay inside the plan section 3.6 envelope
(15k-40k triangles/frame, 150-400 batches, ~40 resident 128x128 PSMT8 textures,
3-6 skinned characters at 1500 tris / 24 bones, 4-6 MB managed heap).

Status: placeholder. Built last, once the pipeline and runtime exist.
TODO(spec missing: section 9) -- the milestone plan scheduling this game is not
yet available.
