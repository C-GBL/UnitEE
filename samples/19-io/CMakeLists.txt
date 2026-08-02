# samples/19-io -- M10 tasks 2/3/4: pads, memory card round trip, and the
# prioritised streaming queue, on the emulated console.

add_executable(19-io main.cpp)

target_link_libraries(19-io PRIVATE
    ps2ur
    pad mc audsrv draw graph math3d packet packet2 dma kernel)
