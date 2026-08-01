# p2b container format

**STUB.** TODO(spec missing: section 10): the binary container specification
is defined by plan section 10, which is not present in `ps2port.txt` (the file
ends at section 8). Do not invent the format; author/recover section 10 first.

What sections 1-8 establish about `.p2b`:

- It is the PS2-native container the exporter emits: `scene.p2b` (entities,
  transforms, component data from SceneExporter) and `assets/*.p2b` (mesh /
  texture / animation / audio output from the converters).
- It is loaded on target by the runtime's `io/` container loader after
  `ps2ur::Init`, before `il2cpp_init` and managed behaviour instantiation.
- Asset payloads are pre-baked for the hardware offline: tri-stripped, VU1-
  batched geometry; palettised + swizzled textures; quantised animation;
  ADPCM (VAG) audio.
- Files are placed on disc in a build-ordered layout (mkps2iso controls LBA
  order) to serve the Addressables-like async load API.

Open questions for section 10: header/chunk layout, endianness/alignment
rules (target is little-endian; DMA wants qword/128-bit alignment),
versioning, string/ID tables, compression, and the scene<->asset reference
scheme.
