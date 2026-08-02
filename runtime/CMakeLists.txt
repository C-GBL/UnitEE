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

# The bridge surface is generated from bridge-api.json (ADR-002). A stale
# generated file is a days-to-find class of bug on hardware, so drift fails
# the configure, not a code review.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../tools/bindgen/bindgen.py" check
        RESULT_VARIABLE PS2UR_BINDGEN_CHECK)
    if(NOT PS2UR_BINDGEN_CHECK EQUAL 0)
        message(FATAL_ERROR "bindgen outputs are stale -- run: python tools/bindgen/bindgen.py gen-all")
    endif()
else()
    message(WARNING "python3 not found; skipping bindgen freshness check")
endif()

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
    src/gfx/gs_batch.cpp
    src/gfx/dma_chain.cpp
    src/vu/vu.cpp
    src/vu/vu_program.cpp
    src/scene/scene.cpp
    src/scene/p2b_scene.cpp
    src/scene/scene_renderer.cpp
    src/scene/scene_load.cpp
    src/gfx/render_queue.cpp
    src/anim/anim.cpp
    src/phys/phys.cpp
    src/phys/phys_shapes.cpp
    src/phys/phys_bvh.cpp
    src/phys/phys_query.cpp
    src/phys/phys_body.cpp
    src/phys/phys_character.cpp
    src/phys/phys_bake.cpp
    src/audio/audio.cpp
    src/io/io.cpp
    src/io/p2b.cpp
    src/io/memcard.cpp
    src/io/stream.cpp
    src/input/input.cpp
    src/bridge/bridge.cpp
    src/bridge/bridge_impl.cpp
    src/bridge/generated_bridge.cpp
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

    # Embedded IOP modules for fileXio (see irx_blobs.S). The assembler needs
    # the ps2sdk irx directory on its include path for the .incbin.
    enable_language(ASM)
    target_sources(ps2ur PRIVATE src/platform/ps2/irx_blobs.S)
    if(DEFINED ENV{PS2SDK})
        set(PS2UR_IRX_DIR "$ENV{PS2SDK}/iop/irx")
    else()
        set(PS2UR_IRX_DIR "C:/Users/Ash/ps2dev/ps2sdk/iop/irx")
    endif()
    set_source_files_properties(src/platform/ps2/irx_blobs.S PROPERTIES
        COMPILE_OPTIONS "-Wa,-I${PS2UR_IRX_DIR}")

    # VU microprograms are EE-target-only: dvp-as output is VU machine code and
    # has no meaning in the host build.
    include(vu)
    add_vu_program(ps2ur SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vu/vu_smoke.vsm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vu/vu_passthrough.vsm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vu/vu_unlit.vsm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vu/vu_lit.vsm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vu/vu_unlit_tex.vsm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vu/vu_lit_fog.vsm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vu/vu_skin.vsm)
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
