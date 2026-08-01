// bridge: the managed <-> native boundary, per ADR-002 (plan section 5).
//
// ABI convention (every symbol in this module follows it):
//   - extern "C", C linkage, never name-mangled
//   - symbol prefix "ps2ur_"
//   - blittable-only parameters and returns: pointers, int8/16/32/64,
//     uint*, float. No C++ types, no structs-by-value across the boundary
//     unless fully blittable and layout-pinned. No doubles (section 3.1).
//   - no exceptions cross the boundary (runtime builds with -fno-exceptions)
//   - the managed side binds with [DllImport("__Internal")]; IL2CPP resolves
//     the symbols at static link time.
//
// Most boundary functions are produced by the binding generator; this file
// holds the hand-written ones.
// TODO(spec missing: section 12): binding generator spec (12.4) and the full
// icall surface for the shim assembly.
#pragma once

namespace ps2ur {
namespace bridge {

bool init();
void shutdown();
bool initialized();

} // namespace bridge
} // namespace ps2ur

extern "C" {

// Example/reference boundary function: managed Debug.Log lands here.
// 'utf8' is a NUL-terminated UTF-8 string owned by the caller for the duration
// of the call. Forwards to ps2ur::log at Info level. Null-safe.
void ps2ur_debug_log(const char* utf8);

} // extern "C"
