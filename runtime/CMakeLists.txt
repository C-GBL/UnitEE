# ps2ur -- PS2-side engine runtime (plan section 8: runtime/).
# Host build (default) exists for unit tests, per plan section 4.3.
cmake_minimum_required(VERSION 3.24)
project(ps2ur C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Target platform: host (x86-64 unit-test build) or ps2 (EE, ps2dev toolchain).
set(PS2UR_PLATFORM "host" CACHE STRING "ps2ur target platform: host or ps2")
set_property(CACHE PS2UR_PLATFORM PROPERTY STRINGS host ps2)

option(PS2UR_BUILD_TESTS "Build host unit tests (host platform only)" ON)

set(PS2UR_SOURCES
    src/core/log.cpp
    src/core/assert.cpp
    src/core/alloc.cpp
    src/core/time.cpp
    src/math/math.cpp
    src/gfx/gfx.cpp
    src/gfx/gs_vram.cpp
    src/gfx/gs_packet.cpp
    src/gfx/gs_device.cpp
    src/gfx/gs_overlay.cpp
    src/gfx/gs_swizzle.cpp
    src/gfx/gs_texcache.cpp
    src/vu/vu.cpp
    src/vu/vu_program.cpp
    src/scene/scene.cpp
    src/anim/anim.cpp
    src/phys/phys.cpp
    src/audio/audio.cpp
    src/io/io.cpp
    src/input/input.cpp
    src/bridge/bridge.cpp
)

if(PS2UR_PLATFORM STREQUAL "host")
    list(APPEND PS2UR_SOURCES src/platform/host/platform_host.cpp)
elseif(PS2UR_PLATFORM STREQUAL "ps2")
    list(APPEND PS2UR_SOURCES src/platform/ps2/platform_ps2.cpp)
else()
    message(FATAL_ERROR "PS2UR_PLATFORM must be 'host' or 'ps2', got '${PS2UR_PLATFORM}'")
endif()

add_library(ps2ur STATIC ${PS2UR_SOURCES})
target_include_directories(ps2ur PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(PS2UR_PLATFORM STREQUAL "ps2")
    target_compile_definitions(ps2ur PUBLIC PS2UR_PLATFORM_PS2=1)

    # VU microprograms are EE-target-only: dvp-as output is VU machine code and
    # has no meaning in the host build.
    include(vu)
    add_vu_program(ps2ur SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vu/vu_smoke.vsm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vu/vu_passthrough.vsm)
    # The ps2 configuration must be driven through the ps2dev cross toolchain
    # file (planned at tools/cmake/ps2-toolchain.cmake, plan section 8), with
    # the toolchain rooted at $ENV{PS2DEV} (default C:/Users/Ash/ps2dev,
    # PS2SDK = $PS2DEV/ps2sdk, GSKIT = $PS2DEV/gsKit). Flags per section 3.1:
    # -G0, -fno-common. TODO(spec missing: section 9): ps2 build milestone.
    if(NOT CMAKE_CROSSCOMPILING)
        message(WARNING "PS2UR_PLATFORM=ps2 without a cross toolchain file; "
                        "this only builds inert stubs.")
    endif()
endif()

# Runtime code is built without exceptions/RTTI (they are dead weight on the EE
# and nothing in the runtime uses them). Tests use compiler defaults.
if(MSVC)
    target_compile_options(ps2ur PRIVATE /EHs-c- /GR- /W4)
else()
    target_compile_options(ps2ur PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra)
endif()

if(PS2UR_PLATFORM STREQUAL "host" AND PS2UR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
