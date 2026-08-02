# samples/21-kart -- the M11 acceptance run: terrain, obstacles, triggers,
# no tunnelling at maximum speed, and the 4 ms physics budget.

add_executable(21-kart main.cpp)

target_link_libraries(21-kart PRIVATE
    ps2ur
    draw graph math3d packet packet2 dma kernel)
