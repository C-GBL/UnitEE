# bindgen — managed <-> native binding generator

Generates both sides of the P/Invoke boundary between the managed shim
(`managed/PS2.UnityShim`) and the native runtime (`runtime/src/bridge`),
per plan section 12.4 and ADR-002. The api-def
(`runtime/src/bridge/bridge-api.json`) is the single source of truth;
generated files are committed and `bindgen.py check` (run at runtime
configure time and in CI) fails when they drift.

## The contract (ADR-002)

- The entire managed <-> native boundary is a set of **`extern "C"`
  functions**, statically linked into the game ELF.
- Managed code reaches them via **`[DllImport("__Internal")]`** P/Invoke —
  the officially supported static-linking mechanism (same as Unity iOS and
  WebGL). IL2CPP resolves the symbols at link time; call cost is roughly a
  direct call.
- Signatures are **blittable-only**: primitive value types and blittable
  structs passed by value or by pointer. No marshalling of strings, arrays,
  or delegates across the boundary except as raw pointers/handles declared in
  the api-def. Object references never cross; opaque `u32` handles do.
- **The generator owns both sides.** Hand-editing generated files is
  forbidden; the api-def is the single source of truth for:
  - the C# `static extern` declarations (`gen-cs`), and
  - the C++ symbol table: `extern "C"` prototypes plus a registration table
    the runtime uses to assert at startup that every imported symbol is
    linked (`gen-cpp`). Implementations live in `runtime/src/bridge` and are
    written by hand against the generated prototypes.

## api-def schema (v0, provisional)

The api-def is a YAML or JSON file. Example:

```yaml
module: ps2ur            # symbol prefix and output grouping
version: 0               # schema version
types:                   # blittable structs shared across the boundary
  - name: P2Vec3
    fields:
      - { name: x, type: f32 }
      - { name: y, type: f32 }
      - { name: z, type: f32 }
functions:
  - name: ps2ur_transform_set_local_position   # exact extern "C" symbol
    cs_class: PS2.UnityShim.Native.TransformNative
    returns: void
    params:
      - { name: handle, type: u32 }            # opaque native handle
      - { name: pos, type: "P2Vec3*" }         # pointer to blittable struct
  - name: ps2ur_time_delta
    cs_class: PS2.UnityShim.Native.TimeNative
    returns: f32
    params: []
```

Primitive type vocabulary: `void`, `u8`, `i8`, `u16`, `i16`, `u32`, `i32`,
`u64`, `i64`, `f32`, `f64`, plus input-only `cstr`. `bool` is rejected with
an explanation (1-byte C++ bool vs 4-byte marshalled BOOL; use `i32`). A
declared struct name may be used by value or, with a trailing `*`, as a
pointer (`dir: out|ref` selects the C# modifier; structs may carry a `cs`
name mapping, e.g. `P2Vec3` -> `UnityEngine.Vector3`). `f64` crossings are
flagged with a warning (soft-float on the EE, plan section 3.1). Anything
else is a generator error — the validator exists so that a non-blittable
signature fails the build on the workstation, not on the console.

## CLI

```
python tools/bindgen/bindgen.py gen-all      # regenerate everything in place
python tools/bindgen/bindgen.py check        # CI freshness gate (exit 1 = stale)
python tools/bindgen/bindgen.py gen-cs  <api-def> -o <dir>
python tools/bindgen/bindgen.py gen-cpp <api-def> -o <dir>
python tools/bindgen/bindgen.py gen-docs <api-def> -o <dir>
```

Outputs: `managed/PS2.UnityShim/Internal/Native.g.cs`,
`runtime/src/bridge/generated_bridge.{h,cpp}` (prototypes, layout
static_asserts, and the startup registration table `bridge::init()`
verifies), and `docs/bridge-surface.md`.
