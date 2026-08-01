# tools/cmake/ps2-toolchain.cmake
#
# CMake cross-toolchain file for the PlayStation 2 Emotion Engine (EE).
#
# Usage:
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=tools/cmake/ps2-toolchain.cmake <src>
#
# Toolchain: ps2dev (native Windows build), tool prefix mips64r5900el-ps2-elf-.
# The install root is taken from the PS2DEV environment variable, defaulting
# to C:/Users/Ash/ps2dev. Per plan section 4.1 the install path must be
# absolute and contain no spaces or non-Latin characters.
#
# Compiler/linker flags follow plan section 3.1:
#   baseline: -D_EE -G0 -O2 -Wall -fno-common
#   C++ adds: -fno-exceptions -fno-rtti

# DEVIATION from plan section 8, which labels this file "CMAKE_SYSTEM_NAME=PS2".
# CMake resolves CMAKE_SYSTEM_NAME to a Modules/Platform/<name>.cmake module;
# no Platform/PS2.cmake exists, so naming it PS2 makes every configure fail
# unless we also ship and register a platform module. Generic is the standard
# CMake spelling for a bare-metal/embedded target and still sets
# CMAKE_CROSSCOMPILING, which is the only property runtime/CMakeLists.txt
# actually gates on. Revisit only if a real Platform/PS2 module becomes useful.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR mips64r5900el)
set(CMAKE_SYSTEM_VERSION 1)

# ---------------------------------------------------------------------------
# Toolchain roots
# ---------------------------------------------------------------------------
if(DEFINED ENV{PS2DEV} AND NOT "$ENV{PS2DEV}" STREQUAL "")
  set(PS2DEV "$ENV{PS2DEV}")
else()
  set(PS2DEV "C:/Users/Ash/ps2dev")
endif()
file(TO_CMAKE_PATH "${PS2DEV}" PS2DEV)

if(DEFINED ENV{PS2SDK} AND NOT "$ENV{PS2SDK}" STREQUAL "")
  set(PS2SDK "$ENV{PS2SDK}")
else()
  set(PS2SDK "${PS2DEV}/ps2sdk")
endif()
file(TO_CMAKE_PATH "${PS2SDK}" PS2SDK)

if("${PS2DEV}" MATCHES " ")
  message(FATAL_ERROR
    "PS2DEV path '${PS2DEV}' contains spaces. The ps2dev toolchain requires "
    "a space-free absolute install path (plan section 4.1).")
endif()

# Convenience platform markers (mirror ps2sdk conventions).
set(PS2 TRUE)
set(EE TRUE)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
set(PS2_TOOL_SUFFIX "")
if(CMAKE_HOST_WIN32)
  set(PS2_TOOL_SUFFIX ".exe")
endif()

set(PS2_TOOL_PREFIX "${PS2DEV}/ee/bin/mips64r5900el-ps2-elf-")

set(CMAKE_C_COMPILER   "${PS2_TOOL_PREFIX}gcc${PS2_TOOL_SUFFIX}")
set(CMAKE_CXX_COMPILER "${PS2_TOOL_PREFIX}g++${PS2_TOOL_SUFFIX}")
set(CMAKE_ASM_COMPILER "${PS2_TOOL_PREFIX}gcc${PS2_TOOL_SUFFIX}")
set(CMAKE_AR           "${PS2_TOOL_PREFIX}ar${PS2_TOOL_SUFFIX}")
set(CMAKE_RANLIB       "${PS2_TOOL_PREFIX}ranlib${PS2_TOOL_SUFFIX}")
set(CMAKE_OBJCOPY      "${PS2_TOOL_PREFIX}objcopy${PS2_TOOL_SUFFIX}")
set(CMAKE_STRIP        "${PS2_TOOL_PREFIX}strip${PS2_TOOL_SUFFIX}")

# Bare-metal target: configure-time test binaries cannot run (or even fully
# link before the SDK layout is verified), so probe with static libraries.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Executables are PS2 ELFs.
set(CMAKE_EXECUTABLE_SUFFIX ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".elf")

# ---------------------------------------------------------------------------
# Flags (plan section 3.1)
# ---------------------------------------------------------------------------
# -ffast-math is deliberate, not a shortcut (plan section 9, M1 task 2): the
# R5900 FPU is already not IEEE 754 -- no NaN, no infinities, overflow
# saturates, denormals flush to zero (section 3.1). Asking the compiler to
# preserve IEEE semantics it cannot deliver only costs code size and speed.
# The deviation is a documented conformance item (section 7.4) and is pinned by
# the conformance suite rather than hidden.
#
# Exceptions/RTTI are off for runtime code. Note that the IL2CPP objects need
# exceptions re-enabled at M6; that is a per-target override, not a change here.
set(PS2_BASE_FLAGS "-D_EE -G0 -O2 -Wall -fno-common -ffast-math")

set(CMAKE_C_FLAGS_INIT   "${PS2_BASE_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${PS2_BASE_FLAGS} -fno-exceptions -fno-rtti")
set(CMAKE_ASM_FLAGS_INIT "${PS2_BASE_FLAGS}")

# ps2sdk headers for every EE compile.
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES
    "${PS2SDK}/ee/include" "${PS2SDK}/common/include")
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
    "${PS2SDK}/ee/include" "${PS2SDK}/common/include")

# ---------------------------------------------------------------------------
# EE link setup
# ---------------------------------------------------------------------------
# RESOLVED 2026-07-31 against the installed toolchain (ps2dev Windows prebuilt,
# GCC 15.2.0 / binutils 2.45.1):
#   - ${PS2SDK}/ee/startup/ contains ONLY 'linkfile'. There is no crt0.o
#     anywhere in the ps2sdk tree -- startup now comes from the gcc driver's
#     own specs, not a hand-linked object.
#   - Linking with just -T<linkfile> produces a working, bootable ELF: verified
#     by cross-building the ps2ur runtime and linking an EE executable against
#     it, then booting it in PCSX2 (EE console output confirmed).
# PS2_LINK_EXPLICIT_CRT0 therefore stays OFF and PS2_CRT0 is kept only as an
# escape hatch for a future SDK layout that reintroduces a standalone crt0.
set(PS2_LINKFILE "${PS2SDK}/ee/startup/linkfile")
set(PS2_CRT0     "${PS2SDK}/ee/startup/crt0.o")

set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-L${PS2SDK}/ee/lib -T${PS2_LINKFILE} -Wl,-zmax-page-size=128")

# ps2sdk libraries available to every EE executable (plan section 9, M1 task 2).
# Targets still name what they use via target_link_libraries -- CMake resolves a
# bare name like `draw` to -ldraw through the -L above. These are the ones the
# linker should always be able to satisfy; libc comes from the compiler's own
# newlib sysroot ($PS2DEV/ee/mips64r5900el-ps2-elf/lib), NOT from ps2sdk/ee/lib.
#
# NOTE: ps2sdk ships both libpacket.a (the original API, used by the SDK's own
# draw samples) and libpacket2.a (the newer one the plan names). Both are
# present; pick per target rather than forcing one here.
set(PS2_SDK_LIBRARIES kernel dma packet packet2 graph draw math3d)

if(NOT DEFINED PS2_LINK_EXPLICIT_CRT0)
  set(PS2_LINK_EXPLICIT_CRT0 OFF)
endif()
if(PS2_LINK_EXPLICIT_CRT0)
  string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " ${PS2_CRT0}")
endif()

# ---------------------------------------------------------------------------
# find_* isolation: never pick up host programs, headers, or libraries
# ---------------------------------------------------------------------------
set(CMAKE_FIND_ROOT_PATH "${PS2DEV}/ee" "${PS2SDK}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
