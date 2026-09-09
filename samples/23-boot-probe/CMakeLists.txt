# samples/23-boot-probe -- the hardware sign-of-life ladder: paints the screen
# at each rung of startup so a black console names the rung that failed.

add_executable(23-boot-probe main.cpp)

target_link_libraries(23-boot-probe PRIVATE
    ps2ur
    draw graph math3d packet packet2 dma patches kernel)
