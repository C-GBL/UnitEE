# samples/16-fog -- M8 task 7: FOGCOL + per-vertex F through vu_lit_fog,
# asserted by monotonic colour slide toward the fog colour with depth.

add_executable(16-fog main.cpp)

target_link_libraries(16-fog PRIVATE
    ps2ur
    draw graph math3d packet packet2 dma kernel)
