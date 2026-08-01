# samples/15-p2b-scene -- M5 acceptance: renders an exported Unity scene.

add_executable(15-p2b-scene main.cpp)

target_link_libraries(15-p2b-scene PRIVATE
    ps2ur
    draw graph math3d packet packet2 dma kernel)
