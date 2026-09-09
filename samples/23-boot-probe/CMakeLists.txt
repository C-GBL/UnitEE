# samples/23-boot-probe -- the hardware sign-of-life ladder: paints the screen
# at each rung of startup so a black console names the rung that failed.

add_executable(23-boot-probe main.cpp)

target_link_libraries(23-boot-probe PRIVATE
    ps2ur
    draw graph math3d packet packet2 dma patches kernel)

# The same ladder inside a load segment the size of the game's (ballast.S),
# for the console that boots the small probe and not the game.
enable_language(ASM)
add_executable(23-boot-probe-big main.cpp ballast.S)
target_compile_definitions(23-boot-probe-big PRIVATE PROBE_BIG=1)
target_link_libraries(23-boot-probe-big PRIVATE
    ps2ur
    draw graph math3d packet packet2 dma patches kernel)
