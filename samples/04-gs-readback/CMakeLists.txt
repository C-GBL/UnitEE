# samples/04-gs-readback -- self-checking GS readback + tile CRC (see main.cpp).

add_executable(04-gs-readback main.cpp)

target_link_libraries(04-gs-readback PRIVATE
    ps2ur
    draw graph math3d packet dma kernel)
