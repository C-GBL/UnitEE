# Host-only unit tests (plan sections 4.3, 8: runtime/tests).
# GoogleTest is fetched at configure time, pinned to release tag v1.14.0.
include(FetchContent)

FetchContent_Declare(
    googletest
    URL "https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# Match the parent project's CRT on MSVC and keep the install tree clean.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(ps2ur_tests
    test_math.cpp
    test_alloc.cpp
    test_alloc_stack_pool.cpp
    test_time.cpp
    test_profiler.cpp
    test_meminfo.cpp
    test_soak.cpp
    test_gfx.cpp
    test_swizzle.cpp
    test_texcache.cpp
    test_batch.cpp
    test_p2b.cpp
    test_world_handles.cpp
    test_render_queue.cpp
    test_anim.cpp
    test_audio.cpp
    test_input.cpp
    test_memcard.cpp
    test_stream.cpp
    test_scene_load.cpp
    test_phys_shapes.cpp
    test_phys_query.cpp
    test_phys_world.cpp
    test_phys_bake.cpp
    test_scene_rigidbody.cpp
    test_scene_ui.cpp
    test_scaled_rig.cpp
)
target_link_libraries(ps2ur_tests PRIVATE ps2ur GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(ps2ur_tests)
