# samples/13-vu1-clip -- M4 task 4: near-plane + guard-band rejection verified.

add_executable(13-vu1-clip main.cpp)

target_link_libraries(13-vu1-clip PRIVATE
    ps2ur
    draw graph math3d packet packet2 dma kernel)
