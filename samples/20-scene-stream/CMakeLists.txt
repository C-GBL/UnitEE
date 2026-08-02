# samples/20-scene-stream -- the M10 acceptance run: async + additive scene
# loading with streamed music playing throughout, no dropouts.

add_executable(20-scene-stream main.cpp)

target_link_libraries(20-scene-stream PRIVATE
    ps2ur
    audsrv draw graph math3d packet packet2 dma kernel)
