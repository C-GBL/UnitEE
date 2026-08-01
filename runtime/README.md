# runtime/ -- "ps2ur", the PS2-side engine

Native runtime skeleton per plan section 8. Public headers live in
`include/ps2ur/`; each subsystem has a stub `init()`/`shutdown()` pair that the
milestone work (TODO(spec missing: section 9)) fills in.

## Host build (unit tests, plan section 4.3)

```
cmake -G Ninja -S "runtime" -B "build/host" -DCMAKE_BUILD_TYPE=Debug
cmake --build "build/host"
ctest --test-dir "build/host" --output-on-failure
```

Quote paths: the repository path contains spaces.

## PS2 build

`-DPS2UR_PLATFORM=ps2` plus the ps2dev cross toolchain file (planned at
`tools/cmake/ps2-toolchain.cmake`). Toolchain root comes from the `PS2DEV`
environment variable, default `C:/Users/Ash/ps2dev` (`PS2SDK = %PS2DEV%/ps2sdk`,
`GSKIT = %PS2DEV%/gsKit`). Until the toolchain is installed the ps2 platform
files are compiling-but-inert stubs.

## Rules

- Single-precision float only; no doubles (plan section 3.1).
- Runtime code builds with `-fno-exceptions -fno-rtti` (MSVC: `/EHs-c- /GR-`).
- Never copy Unity-owned sources (libil2cpp etc.) into this tree (plan section 8).
