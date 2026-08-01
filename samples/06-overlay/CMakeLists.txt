# samples/06-overlay -- M2 task 7: debug text overlay, verified via readback.

add_executable(06-overlay main.cpp)

target_link_libraries(06-overlay PRIVATE
    ps2ur
    draw graph math3d packet dma kernel)
