# samples/02-scene-graph -- M8 acceptance: 500 objects through the full
# cull/sort/material path at 30 fps, goldens from 8 camera poses.

add_executable(02-scene-graph main.cpp)

target_link_libraries(02-scene-graph PRIVATE
    ps2ur
    draw graph math3d packet packet2 dma kernel)
