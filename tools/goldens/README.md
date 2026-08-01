# goldens — golden-image capture and compare

Tooling for the D7 definition-of-done criterion (plan section 2):
"Rendering golden-image tests pass at 100% for the reference scene set,
verified by framebuffer CRC diff via PCSX2."

> TODO(spec missing: section 14.3): the plan references the full
> golden-image capture/compare specification in section 14.3, which is not
> present in `ps2port.txt` (the document ends at section 8). This directory
> holds the plan stub until that section is recovered or rewritten.

## Approach (derived from sections 2 and 4.1)

- Each reference scene is built to an ELF and run headless under PCSX2 using
  its CLI switches (plan section 4.1): `-batch -nogui -elf <file>
  -logfile <path> -fastboot`.
- The test build of the runtime renders a fixed number of frames with a
  fixed timestep and deterministic input, computes a CRC of the final
  framebuffer contents on-target, and prints it to the console log
  (`GOLDEN <scene> <frame> <crc>` lines).
- The compare script parses the PCSX2 log and diffs the CRCs against checked
  in golden values stored in this directory, one file per scene.
- A golden update is an explicit, reviewed action (a `--rebless` mode), never
  automatic.

## Open questions for the missing spec

- TODO(spec missing: section 14.3): exact CRC algorithm and whether the CRC
  is computed EE-side (framebuffer readback via local memory transfer) or
  from a PCSX2 screenshot/dump.
- TODO(spec missing: section 14.3): tolerance policy — plan section 2 says
  100% pass, which implies bit-exact CRCs and therefore a pinned PCSX2
  version (record it in `tools/ps2dev/PINNED.md`).
- TODO(spec missing: section 14.3): scene list for "the reference scene set"
  (presumably `samples/00..02` plus kart-demo views).
