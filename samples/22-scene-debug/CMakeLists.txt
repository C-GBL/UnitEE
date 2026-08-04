# samples/22-scene-debug -- render diagnostics for any exported container:
# loads debugscene.p2b through the production render path and logs why each
# entity was culled, queued, and bound.

add_executable(22-scene-debug main.cpp)

target_link_libraries(22-scene-debug PRIVATE
    ps2ur
    draw graph math3d packet packet2 dma kernel)
