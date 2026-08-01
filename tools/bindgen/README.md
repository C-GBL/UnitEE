# bindgen — managed <-> native binding generator

Generates both sides of the P/Invoke boundary between the managed shim
(`managed/PS2.UnityShim`) and the native runtime (`runtime/src/bridge`).

> TODO(spec missing: section 12.4): the plan references a full binding
> generator specification in section 12.4, which is not present in
> `ps2port.txt` (the document ends at section 8). Everything below is derived
> from ADR-002 (plan section 5) and the repository layout (plan section 8).
> `bindgen.py` is a CLI skeleton until section 12.4 is recovered or rewritten.

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

Primitive type vocabulary: `void`, `bool` (marshalled as `u8`), `u8`, `i8`,
`u16`, `i16`, `u32`, `i32`, `u64`, `i64`, `f32`, `f64`. A declared struct
name may be used by value or, with a trailing `*`, as a pointer. `f64`
crossings are flagged with a warning (soft-float on the EE, plan section
3.1). Anything else is a generator error — the validator exists so that a
non-blittable signature fails the build on the workstation, not on the
console.

## CLI

```
python tools/bindgen/bindgen.py gen-cs  <api-def> -o <out-dir>   # C# externs
python tools/bindgen/bindgen.py gen-cpp <api-def> -o <out-dir>   # C++ headers + symbol table
```

Both subcommands currently parse and validate the api-def, then exit with
status 2 and "not implemented" (see the TODO above).
