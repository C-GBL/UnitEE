# samples/14-vu1-chain -- M4 tasks 5/6/7 + acceptance parity (VU1 vs immediate).

add_executable(14-vu1-chain main.cpp)

target_link_libraries(14-vu1-chain PRIVATE
    ps2ur
    draw graph math3d packet packet2 dma kernel)
