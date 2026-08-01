# il2cpp --convert-to-cpp smoke test (Unity 6000.0.47f1)

Date: 2026-07-31. Empirical verification of plan section 1.2 item 1: that the
`il2cpp` executable shipped with the local Editor converts a .NET assembly to
portable C++ standalone, without `--compile-cpp`. This de-risks ADR-001
option B (IL2CPP + reimplemented runtime).

Scratch artifacts live in `build/il2cpp-smoke/` (gitignored per section 8).

## 1. Resolved [VERIFY] items

- Plan section 1.2 says the compiler lives at
  `Editor/Data/il2cpp/build/deploy/net*/il2cpp[.exe]` with a `[VERIFY]` on the
  netX.0 folder name. On 6000.0.47f1 there is NO net* subfolder; the binary is
  directly at:
  `C:/Program Files/Unity/Hub/Editor/6000.0.47f1/Editor/Data/il2cpp/build/deploy/il2cpp.exe`
  It is a NativeAOT-compiled single executable (stack traces show `+ 0xNN`
  offsets, no managed il2cpp.dll next to it), accompanied by the Bee build
  system DLLs and, notably, `UnityLinker.exe` (the managed stripper) in the
  same `deploy/` directory.
- Plan section 4.2 references `unityaot-linux` mscorlib with `[VERIFY]`. This
  install ships three AOT BCL flavors under
  `Editor/Data/MonoBleedingEdge/lib/mono/`: `unityaot-win32`,
  `unityaot-linux`, `unityaot-macos` (plus Facades/ in each). The smoke test
  used `unityaot-win32`.

## 2. Option discovery

`il2cpp.exe --help` prints a full option list. Relevant subset:

```
--convert-to-cpp                    Convert the provided assemblies to C++
--compile-cpp                       Compile generated C++ code
--assembly=<path,path,..>           One or more paths to assemblies to convert
--directory=<path,path,..>          One or more directories containing assemblies to convert
--extra-types-file=<path,path,..>   Extra generic instance types to include
--generatedcppdir=<path>            The directory where generated C++ code is written
--additional-cpp=<path,path,..>     Additional C++ files to include
--emit-null-checks                  Enables generation of null checks
--enable-array-bounds-check         Enables generation of array bounds checks
--enable-divide-by-zero-check       Enables generation of divide by zero checks
--enable-stacktrace                 Stacktrace sentries (for platforms w/o stack walk APIs)
--emit-comments                     Annotate generated code with comments
--disable-generic-sharing           Disables generic sharing
--maximum-recursive-generic-depth=<value>   (default 7)
--code-generation-option=<value>    Code generation tuning
--file-generation-option=<value>    File output tuning
--generics-option=<value>           Generics tuning
--incremental-g-c-time-slice=<value>  Incremental GC time slice (ms)
--data-folder=<path>                Where non-source data is written
--cachedirectory=<path>             Cache dir for conversion
--jobs=<value>                      Core count for conversion/compilation
--dont-deploy-baselib               il2cpp will not use its own baselib
--additional-defines / --additional-include-directories / --compiler-flags /
--linker-flags / --outputpath / --configuration   (compile-cpp side; unused here)
```

HIDDEN OPTIONS (not shown by --help, discovered via string analysis of the
NativeAOT binary and confirmed empirically):

- `--dotnetprofile=<name>` -- REQUIRED for conversion on this version. Valid
  names (from the error message when given a wrong value): `net45`, `net7.0`,
  `unityaot-linux`, `unityaot-macos`, `unityaot-win32`,
  `unitycoreclr-linux-x64`, `unitycoreclr-osx-arm64`, `unitycoreclr-osx-x64`,
  `unitycoreclr-windows-x64`, `unityjit`. The legacy family name `unityaot`
  alone is NOT accepted; it must be fully qualified.
- `--platform=<name>` -- exists as a parsed field but passing
  `--platform=WindowsDesktop` did NOT qualify the default `unityaot` profile
  in our tests (same error with or without it). Platform names embedded in the
  binary include WindowsDesktop, Linux, MacOSX, Android, iOS, WebGL, PS4, PS5,
  Switch, GameCore*, EmbeddedLinux, Qnx. For our pipeline the reliable knob is
  the fully qualified `--dotnetprofile`; `--platform` appears to matter for
  the `--compile-cpp` path we do not use.

There is no switch for a PS2 platform, as expected: conversion output is
platform-portable C++ and the platform specialization happens when compiling
it against a patched libil2cpp (plan section 1.2 item 2).
TODO(spec missing: section 11): the IL2CPP port spec that defines which
IL2CPP_TARGET_* macros and profile we standardize on is referenced by the plan
but absent from ps2port.txt; this note records only what was proven.

## 3. Test assembly

`build/il2cpp-smoke/HelloIl2cpp.cs`: static entry method, a generic method
with an `IComparable<T>` constraint, `List<int>` + `foreach`, a user struct
with an operator, string interpolation, lambda + anonymous delegate through a
user delegate type.

Compiled against the unityaot BCL with the dotnet SDK's Roslyn (exact working
command, first try, no errors):

```
dotnet "C:/Program Files/dotnet/sdk/9.0.304/Roslyn/bincore/csc.dll" ^
  -noconfig -nostdlib -deterministic -langversion:9 -target:library ^
  -out:HelloIl2cpp.dll ^
  -r:"C:/Program Files/Unity/Hub/Editor/6000.0.47f1/Editor/Data/MonoBleedingEdge/lib/mono/unityaot-win32/mscorlib.dll" ^
  HelloIl2cpp.cs
```

mscorlib.dll alone was sufficient; in the Mono-derived unityaot BCL,
`List<T>`, `IComparable<T>`, and `string.Format` all live in mscorlib.
`System.dll` / `System.Core.dll` exist in the same directory when needed.

## 4. Conversion: errors encountered and fixes, in order

1. Bare invocation
   (`--convert-to-cpp --assembly=HelloIl2cpp.dll --generatedcppdir=out`):
   `System.InvalidOperationException: A platform must be provided to obtain a
   UnityAot profile.` The default profile name is the unqualified family
   `unityaot`, which needs a platform to resolve. Fix: pass the hidden,
   fully qualified `--dotnetprofile=unityaot-win32`.
2. `--dotnetprofile=unityaot` (unqualified, with any `--platform` value):
   `System.ArgumentException: Unknown runtime profile : unityaot. Available
   profiles were : net45, net7.0, unityaot-linux, unityaot-macos,
   unityaot-win32, unitycoreclr-*, unityjit`. Fix: qualify the name.
3. `--dotnetprofile=unityaot-win32` with only our assembly:
   `Unity.IL2CPP.DataModel.MissingAssemblyException: mscorlib was not resolved
   up front`. il2cpp does not probe for references; every referenced assembly
   must be an explicit input. Fix: add mscorlib.dll (in place, from the Unity
   install -- no copying needed) to `--assembly`.
4. After that: success, exit code 0. Only diagnostic: a warning about high
   instruction count in mscorlib's RIPEMD160Managed (harmless).

## 5. Exact working il2cpp command

```
"C:/Program Files/Unity/Hub/Editor/6000.0.47f1/Editor/Data/il2cpp/build/deploy/il2cpp.exe" ^
  --convert-to-cpp ^
  --dotnetprofile=unityaot-win32 ^
  --emit-null-checks --enable-array-bounds-check --enable-divide-by-zero-check ^
  --emit-comments ^
  --assembly="C:/Users/Ash/Documents/Unity 2 PS2/build/il2cpp-smoke/HelloIl2cpp.dll,C:/Program Files/Unity/Hub/Editor/6000.0.47f1/Editor/Data/MonoBleedingEdge/lib/mono/unityaot-win32/mscorlib.dll" ^
  --generatedcppdir="C:/Users/Ash/Documents/Unity 2 PS2/build/il2cpp-smoke/out"
```

Exit 0. Wall time 5.7 s (cold, includes converting ALL of mscorlib).

## 6. Output statistics

In `--generatedcppdir` (with `--emit-comments`):

- 98 files: 82 `.cpp`, 13 `.c`, 2 `.dat`, 1 `.json` (diagnostics manifest).
- Source total ~152 MB with comments; ~133 MB without. Almost all of it is
  mscorlib codegen (`Generics__*.cpp`, `GenericMethods__*.cpp`,
  `mscorlib*.cpp`); our assembly produced `HelloIl2cpp.cpp` (45 KB) +
  `HelloIl2cpp_CodeGen.c` (3.5 KB).
- `Data/Metadata/global-metadata.dat` (3.4 MB) and
  `Data/Resources/mscorlib.dll-resources.dat` (374 KB) are emitted at
  CONVERSION time -- the PS2 pipeline must embed/ship these with the ELF/ISO
  and the loader must hand them to libil2cpp at init.
- No headers are generated; every generated file includes `pch-cpp.hpp` /
  `pch-c.h`, which live in `Editor/Data/il2cpp/libil2cpp/pch/`. Generated
  code compiles only against libil2cpp headers (fetched to `build/` by
  `il2cpp-port/apply.py` per the section 8 hard rule, never committed).

## 7. Generated code spot-check

`HelloIl2cpp.cpp` is the expected portable IL2CPP codegen:

- `Il2Cpp*` machinery throughout: `IL2CPP_EXTERN_C RuntimeClass*
  ..._il2cpp_TypeInfo_var`, `const RuntimeMethod*` vars, string literal vars.
- Method shape: `IL2CPP_EXTERN_C IL2CPP_METHOD_ATTR int32_t
  Hello_Entry_mAB1C44CC... (const RuntimeMethod* method)` with lazy per-method
  metadata init (`il2cpp_codegen_initialize_runtime_metadata`).
- Safety flags took effect: 19 `NullCheck` sites and
  `IL2CPP_ARRAY_BOUNDS_CHECK(index, ...)` in array accessors.
- Generic sharing is on by default (shared `Generics__*.cpp` bodies +
  per-instantiation thin entries); `List<int>`, the generic `Max<T>`, both
  delegate flavors, the struct, and interpolation all round-tripped.
- Plain C (`.c`) files carry the metadata tables (`Il2CppTypeDefinitions.c`,
  `Il2CppGenericInstDefinitions.c`, etc.).

## 8. Verdict

`--convert-to-cpp` WORKS STANDALONE on 6000.0.47f1. Plan section 1.2 item 1
is confirmed on this exact install, with two version-specific deltas: the
binary sits directly in `deploy/` (no net* folder), and the conversion
requires the hidden, fully qualified `--dotnetprofile=unityaot-<os>` switch
plus explicit up-front `--assembly` listing of every referenced BCL assembly.
ADR-001 option B's first dependency is therefore de-risked: we can produce
portable C++ from user C# using only shipped, invocable tooling, with no
Unity Editor process in the loop.

Implications for the build pipeline's RunIl2cpp stage:

1. Inputs must be a CLOSED assembly set: user assemblies + shim + every
   unityaot BCL assembly they reference, passed via `--assembly` (or
   `--directory`). No probing happens.
2. Pin `--dotnetprofile=unityaot-win32` (host-OS BCL; output is portable
   C++ either way, but pin ONE and record it -- the three unityaot flavors
   are not guaranteed IL-identical).
3. Run `UnityLinker.exe` (same deploy dir) BEFORE il2cpp to strip the BCL;
   unstripped mscorlib alone is ~130 MB of C++, which would dominate EE-side
   compile time and code size. TODO(spec missing: section 12.3): the Roslyn
   recompilation pass spec that feeds this stage is referenced but absent
   from ps2port.txt. TODO(spec missing: section 9): the M-gate that sizes
   stripped output against the 32 MB budget is in the missing milestone plan.
4. `--generatedcppdir` output plus `Data/` is the complete handoff artifact:
   sources -> mips64r5900el-ps2-elf-g++ + patched libil2cpp;
   `global-metadata.dat` -> ISO payload loaded at `il2cpp_init`.
5. Useful knobs verified working: `--emit-null-checks`,
   `--enable-array-bounds-check`, `--enable-divide-by-zero-check`,
   `--emit-comments`; also relevant later: `--extra-types-file`,
   `--maximum-recursive-generic-depth`, `--disable-generic-sharing`,
   `--jobs`, `--cachedirectory`.
6. Conversion is fast (seconds); the D6 build-time budget will be dominated
   by the native compile, not by il2cpp conversion.
