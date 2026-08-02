# samples/17-skinning -- M9 acceptance: three skinned characters with
# crossfades at 30 fps, checked against an Editor-sampled pose trace.

add_executable(17-skinning main.cpp)

target_link_libraries(17-skinning PRIVATE
    ps2ur
    draw graph math3d packet packet2 dma kernel)
