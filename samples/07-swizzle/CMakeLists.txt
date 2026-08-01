# samples/07-swizzle -- M3 task 1: proves PSMT8 swizzle + CSM1 CLUT order on target.

add_executable(07-swizzle main.cpp)

target_link_libraries(07-swizzle PRIVATE
    ps2ur
    draw graph math3d packet dma kernel)
