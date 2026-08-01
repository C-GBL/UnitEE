# ci — continuous integration plan

Plan stub. The plan references milestone-level CI gates (sections 2 and 9)
and testing specifications (section 14), but sections 9-18 are missing from
`ps2port.txt` (the document ends at section 8).
TODO(spec missing: sections 9, 14): replace this stub with concrete pipeline
definitions once those sections are recovered or rewritten.

## Stages

1. **Managed build** — build `managed/PS2.UnityShim`, `PS2.UnityShim.Editor`
   and `PS2.Conformance` with the dotnet SDK; run bindgen validation against
   the api-def; run Roslyn-level analyzer/validator checks.
2. **Host runtime tests** — x86-64 build of the ps2ur runtime
   (`runtime/src/platform/host`) with GoogleTest (plan section 4.3: "not
   optional"); run unit tests and the host-side conformance harness.
3. **PS2 cross build** — CMake + Ninja with
   `tools/cmake/ps2-toolchain.cmake`; produces `game.elf` for the samples;
   checks D6 build-time budgets (plan section 2) once measurable.
4. **PCSX2 boot check** — run the built ELF headless (`-batch -nogui -elf`,
   plan section 4.1), assert clean boot from the log; later stages add the
   golden-image CRC diff (`tools/goldens`) and frame-time capture for D2.

The one-click D1 criterion additionally needs a headless Unity Editor stage
(`-batchmode -executeMethod`) on a Windows runner with Unity 6000.0.47f1
installed; that is out of scope for this stub.

## Toolchain provisioning

- **CI**: the `ps2dev/ps2dev` Docker image, pinned **by digest** — never
  `:latest` (plan section 4.1, option 1). Record the chosen digest in
  `tools/ps2dev/PINNED.md` and bump it deliberately. Note: Docker is not
  available on the current workstation; the containerized path is for hosted
  CI runners only.
- **Workstations**: the native Windows release tarball, installed by
  `tools/ps2dev/install.ps1` (which appends the pin record automatically).
- Plan section 8 places a Dockerfile under `tools/ps2dev/`;
  TODO(spec missing: sections 9, 14): add it when the CI runner
  requirements are specified.
