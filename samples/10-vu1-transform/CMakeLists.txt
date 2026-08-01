# samples/10-vu1-transform -- M4 task 2: verifies the VU1 transform maths.

add_executable(10-vu1-transform main.cpp)

target_link_libraries(10-vu1-transform PRIVATE
    ps2ur
    draw graph math3d packet packet2 dma kernel)
