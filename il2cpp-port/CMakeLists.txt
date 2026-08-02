# EE build of libil2cpp + IL2CPP-generated code (plan section 9, M6 task 5).
#
# Standalone project, configured with the ps2 toolchain file against the
# STAGED tree in build/il2cpp (never the Unity install, never committed):
#
#   cmake -G Ninja -S il2cpp-port -B build/m6-ee \
#     -DCMAKE_TOOLCHAIN_FILE=tools/cmake/ps2-toolchain.cmake \
#     -DM6_GENERATED=<dir with il2cpp --convert-to-cpp output>
#
# Sources are globbed at configure time because the staged tree is produced by
# apply.py, not authored here; re-run cmake after apply.py.
cmake_minimum_required(VERSION 3.24)
project(m6ee C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(L "${CMAKE_CURRENT_SOURCE_DIR}/../build/il2cpp/libil2cpp")
if(NOT EXISTS "${L}/il2cpp-config.h")
    message(FATAL_ERROR "staged libil2cpp missing -- run: python il2cpp-port/apply.py prepare")
endif()
set(M6_GENERATED "" CACHE PATH "il2cpp --convert-to-cpp output directory")
if(NOT EXISTS "${M6_GENERATED}")
    message(FATAL_ERROR "M6_GENERATED not set or missing: '${M6_GENERATED}'")
endif()

# Host program for the ELF. main_ee.cpp is the M6 gate program (pure managed
# hello); main_m7.cpp drives the managed facade against the ps2ur renderer
# and needs PS2UR_RUNTIME pointing at a ps2-platform build of the runtime
# (its libps2ur.a carries the VU microprograms as linkable blobs).
set(M6_MAIN "main_ee.cpp" CACHE STRING "host program source")
set(PS2UR_RUNTIME "" CACHE PATH "ps2 runtime build dir containing libps2ur.a (optional)")

# Platform-neutral libil2cpp sources; per-platform subdirectories are excluded
# wholesale (their guards would no-op them anyway, but excluding is faster and
# keeps the error surface small).
file(GLOB LIB_SOURCES
    "${L}/*.cpp"
    "${L}/gc/*.cpp"
    "${L}/mono/ThreadPool/*.cpp"
    "${L}/utils/*.cpp"
    "${L}/vm/*.cpp"
    "${L}/vm-utils/*.cpp"
    "${L}/metadata/*.cpp"
    "${L}/codegen/*.cpp"
    "${L}/debugger/*.cpp"
    "${L}/os/*.cpp"
    "${L}/os/Generic/*.cpp"
    "${L}/os/ClassLibraryPAL/*.cpp"
    "${L}/os/c-api/*.cpp"
    "${L}/os/ps2/*.cpp"
    "${L}/icalls/mscorlib/*/*.cpp"
    "${L}/icalls/System/*/*.cpp"
    "${L}/icalls/System.Core/*/*.cpp"
    # Interlocked and Math icalls live under vm-utils, not icalls.
    "${L}/vm-utils/icalls/*/*/*.cpp"
)

file(GLOB GEN_SOURCES "${M6_GENERATED}/*.cpp" "${M6_GENERATED}/*.c")

# bdwgc as Unity builds it: the extra/gc.c amalgam, one translation unit.
# Defines mirror libil2cpp/gc/gc_wrapper.h ("here is the defines we build
# Boehm with") plus the single-threaded/bare-metal reductions; the PS2
# platform stanza itself lives in the gcconfig.h patch.
set(BDWGC "${CMAKE_CURRENT_SOURCE_DIR}/../build/il2cpp/bdwgc")

add_executable(m6-hello
    ${LIB_SOURCES}
    ${GEN_SOURCES}
    "${BDWGC}/extra/gc.c"
    "${M6_MAIN}"
)

if(PS2UR_RUNTIME)
    target_include_directories(m6-hello PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include")
    # Same ps2sdk library set the samples use; the runtime's GS/DMA/VU code
    # sits on top of them.
    target_link_libraries(m6-hello PRIVATE
        "${PS2UR_RUNTIME}/libps2ur.a"
        draw graph math3d packet packet2 dma
        # M10 added platform services to the runtime, so anything linking
        # libps2ur.a now needs their SDK libraries too: pad (input), mc
        # (memory card) and audsrv (audio). Without these the link fails on
        # padInit/mcInit/audsrv_init long after a 65-second compile.
        pad mc audsrv)
endif()

set_source_files_properties("${BDWGC}/extra/gc.c" PROPERTIES COMPILE_DEFINITIONS
    "GC_GCJ_SUPPORT=1;IGNORE_DYNAMIC_LOADING=1;GC_DONT_REGISTER_MAIN_STATIC_DATA=1;ALL_INTERIOR_POINTERS=1")

set(BASELIB "${CMAKE_CURRENT_SOURCE_DIR}/../build/il2cpp/baselib")
set(EXTERNAL "${CMAKE_CURRENT_SOURCE_DIR}/../build/il2cpp")
target_include_directories(m6-hello PRIVATE
    "${L}"
    "${L}/pch"
    "${L}/os/ps2"
    "${L}/os/ps2/baselib"
    "${BASELIB}/Include"
    # Platform .inl.h set: WebGL's no-pthreads variant is the closest shipped
    # model for a single-threaded console; our own headers above take
    # precedence where they exist. Referenced in place -- Unity's files are
    # never copied into the repository.
    "${BASELIB}/Platforms/WebGL/Include"
    "${BDWGC}/include"
    "${M6_GENERATED}"
)

target_compile_definitions(m6-hello PRIVATE
    IL2CPP_USE_PLATFORM_CONFIG=1
    # il2cpp forward-declares baselib types inside inline namespace
    # il2cpp_baselib; this is how Unity's own build invokes baselib.
    BASELIB_INLINE_NAMESPACE=il2cpp_baselib
    NDEBUG
    # The unityaot-win32 BCL carries DllImport("kernel32.dll") and friends;
    # forcing them internal makes every such call a direct link-time call
    # into os/ps2/Win32PInvokeShims.cpp (the console-platform mechanism).
    # A shim gap then fails the LINK instead of throwing on target. libc is
    # deliberately not forced: its declaration (snprintf) would clash with
    # newlib's real prototype inside the same translation unit.
    FORCE_PINVOKE_kernel32_INTERNAL=1
    FORCE_PINVOKE_Kernel32_INTERNAL=1
    FORCE_PINVOKE_advapi32_INTERNAL=1
    FORCE_PINVOKE_BCrypt_INTERNAL=1
    FORCE_PINVOKE_api_ms_win_core_timezone_l1_1_0_INTERNAL=1
    FORCE_PINVOKE_user32_INTERNAL=1
    # The generated code and runtime are third-party output: warnings are
    # theirs to own, not noise for this build.
)

# Exceptions ON for everything here (managed try/catch lowers to C++ EH --
# verified working on the EE in the M6 pre-flight). RTTI stays off, matching
# how Unity builds libil2cpp. -w: third-party code, warnings are not ours.
# -Os: measured 6.75 MB text at -O2; size is the M6 gate risk (plan section
# 15), and the Vector3 budget passes with 9x headroom, so size wins.
target_compile_options(m6-hello PRIVATE -fexceptions -fno-rtti -w -Os
    -ffunction-sections)
target_link_options(m6-hello PRIVATE -Wl,--gc-sections
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/keep-eh.ld")

# MonoIO.cpp passes MonoIOError* (int32_t*) into os::File's int* parameters
# at ~14 sites. On this ABI int32_t is `long int`, distinct from `int` but the
# same 32 bits; -fpermissive on that ONE third-party file beats a 14-hunk
# patch that upstream would express differently anyway. Recorded in the port
# notes.
set_source_files_properties(
    "${L}/icalls/mscorlib/System.IO/MonoIO.cpp"
    "${L}/icalls/mscorlib/System.IO/BrokeredFileSystem.cpp"
    "${L}/os/Messages.cpp"
    "${L}/mono/ThreadPool/threadpool-ms-io.cpp"
    PROPERTIES COMPILE_OPTIONS "-fpermissive")

# The generated registration table guards g_CodegenRegistration behind
# RUNTIME_IL2CPP (set when il2cpp is the runtime, as opposed to feeding the
# Unity engine build). Scoped to the one file that consumes it.
set_source_files_properties(
    "${M6_GENERATED}/Il2CppCodeRegistration.cpp"
    PROPERTIES COMPILE_DEFINITIONS "RUNTIME_IL2CPP=1")

# Split optimization (measured, docs/m6-report.md): -Os on the USER assembly
# costs 17x on the Vector3 benchmark (27 ms -> 468 ms, past the 250 ms gate)
# because the hot struct calls stop inlining. BCL and runtime stay -Os for
# size; user-assembly and shim translation units get -O2 (appended last, so
# it wins) -- script code and Transform property plumbing are the hot side.
file(GLOB M6_USER_ASSEMBLY_SOURCES
    "${M6_GENERATED}/HelloEE*.cpp"
    "${M6_GENERATED}/Assembly-CSharp*.cpp"
    "${M6_GENERATED}/PS2.UnityShim*.cpp")
set_source_files_properties(${M6_USER_ASSEMBLY_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-O2")

target_link_libraries(m6-hello PRIVATE kernel)
