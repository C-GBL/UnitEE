# samples/03-sdk-reference -- ps2sdk-only control for GS bring-up (see main.c).
# Deliberately does NOT link ps2ur.

add_executable(03-sdk-reference main.c)

target_link_libraries(03-sdk-reference PRIVATE
    draw graph math3d packet dma kernel)
