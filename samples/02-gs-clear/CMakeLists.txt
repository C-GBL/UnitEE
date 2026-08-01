# samples/02-gs-clear -- isolation harness for the clear path (see main.cpp).

add_executable(02-gs-clear main.cpp)

target_link_libraries(02-gs-clear PRIVATE
    ps2ur
    draw graph math3d packet dma kernel)
