# ADR-002: How do managed scripts call into the native runtime?

Status: Accepted (plan section 5, ADR-002)

## Context

User scripts compile against `PS2.UnityShim`, a Unity-shaped facade assembly.
Every engine-touching call (transform reads/writes, component lookups,
rendering, input, audio, ...) must cross from IL2CPP-generated C++ into the
ps2ur runtime, cheaply and stably across Unity versions.

## Options

- **Implement Unity's real internal calls against real `UnityEngine.dll`.**
  **Rejected.** Thousands of icalls; redistributing/relying on Unity's
  assemblies for a foreign runtime is legally and practically untenable.
- **`[MethodImpl(InternalCall)]` + custom icall registration.** Rejected.
  IL2CPP's icall resolution is internal machinery that changes between
  versions; fragile.
- **`[DllImport("__Internal")]` P/Invoke to statically linked C functions.**
  **Chosen.** This is the officially supported mechanism on iOS and WebGL, it
  is stable across Unity versions, IL2CPP resolves the symbols at link time,
  and it costs roughly a direct call.

## Decision

All managed->native calls are `[DllImport("__Internal")]` P/Invokes resolved
at static link time against `extern "C"` functions in `runtime/src/bridge/`.

## Boundary type policy

"Blittable-only" left two questions open that the shim, the bindgen validator,
and this ADR each answered differently. Resolved 2026-07-31; this section is
the single source of truth, and `tools/bindgen/bindgen.py` enforces it.

| Type | Verdict |
|---|---|
| Integers (`u8`..`i64`), `f32` | Allowed anywhere. |
| `f64` | Allowed, but bindgen **warns**: soft-float on the EE, 20-100x slower (plan section 3.1). |
| Blittable structs of the above | Allowed anywhere, declared in the api-def's `types`. |
| Pointers / handles / IDs | Allowed. Ownership is documented per function. |
| `bool` | **Forbidden.** C++ `bool` is 1 byte; the CLR marshals `System.Boolean` as the 4-byte Win32 `BOOL` by default. The two sides silently disagree about width. Use `i32` with 0/non-0 semantics. |
| `cstr` (`const char*` from `System.String`) | **Allowed as an input parameter only**, and only on diagnostic paths (logging, assertions, profiler labels) where a per-call copy is irrelevant. Never a return type, never a struct field: that would make the native side own an allocation the managed side must free, and this boundary defines no protocol for that. |
| Everything else (classes, delegates, arrays, `string` out/return) | Forbidden. |

## Consequences

- The entire managed<->native boundary is a set of `extern "C"` functions
  with **blittable-only signatures** (no marshalling of managed objects;
  handles/IDs and blittable structs only), plus the narrowly scoped `cstr`
  input exception above.
- A binding generator owns BOTH sides of the boundary -- the C# externs in
  the shim and the C symbols in `bridge/` -- so they cannot drift
  (TODO(spec missing: section 12.4): bindgen spec; lives in
  `tools/bindgen/`).
- `bridge/` contains generated files plus hand-written ones (plan section 8).
- Hot-path API design must respect call granularity: prefer batched/struct
  calls over chatty per-field crossings.
