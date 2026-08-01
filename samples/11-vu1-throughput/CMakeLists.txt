# samples/11-vu1-throughput -- M4 acceptance measurement (tris/frame, fps).

add_executable(11-vu1-throughput main.cpp)

target_link_libraries(11-vu1-throughput PRIVATE
    ps2ur
    draw graph math3d packet packet2 dma kernel)
