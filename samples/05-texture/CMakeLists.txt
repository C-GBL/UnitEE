# samples/05-texture -- M2 task 6: texture upload, drawn and verified via readback.

add_executable(05-texture main.cpp)

target_link_libraries(05-texture PRIVATE
    ps2ur
    draw graph math3d packet dma kernel)
