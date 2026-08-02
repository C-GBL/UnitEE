# samples/18-audio -- M10 task 1: SPU2 clips, voice stealing, 3D
# attenuation and streamed music, verified on the emulated hardware.

add_executable(18-audio main.cpp)

target_link_libraries(18-audio PRIVATE
    ps2ur
    audsrv draw graph math3d packet packet2 dma kernel)
