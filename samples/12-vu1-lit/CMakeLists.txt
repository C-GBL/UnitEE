# samples/12-vu1-lit -- M4 task 3: verifies VU1 per-vertex lighting.

add_executable(12-vu1-lit main.cpp)

target_link_libraries(12-vu1-lit PRIVATE
    ps2ur
    draw graph math3d packet packet2 dma kernel)
