# unity-ps2 -- top-level native build (plan section 9, M1).
#
# Two targets from one tree (plan section 14.1, "the highest-leverage
# investment"): the EE cross build, and a host x86-64 build that runs the same
# code under a test harness and sanitisers.
#
#   cmake --preset ps2-release  && ninja -C build/ps2-release
#   cmake --preset host-debug   && ninja -C build/host-debug && ctest --test-dir build/host-debug
#
# The ps2 presets supply tools/cmake/ps2-toolchain.cmake; PS2DEV comes from the
# environment (see tools/ps2dev/env.ps1) and defaults to C:/Users/Ash/ps2dev.
cmake_minimum_required(VERSION 3.24)
project(unity_ps2 C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake")

# enable_testing() must be called at the TOP level, not just in runtime/:
# CTest only writes a root CTestTestfile.cmake (the one that recurses into
# subdirectories) for the directory where testing was enabled. With it only in
# runtime/, `ctest --preset host-debug` reports "No tests were found" even
# though the test executable built fine.
enable_testing()

add_subdirectory(runtime)

# Samples are PS2 executables; there is nothing to build for them on the host.
if(PS2UR_PLATFORM STREQUAL "ps2")
    add_subdirectory(samples/00-hello-triangle)
    add_subdirectory(samples/01-spinning-cube)
    add_subdirectory(samples/02-gs-clear)
    add_subdirectory(samples/03-sdk-reference)
endif()
