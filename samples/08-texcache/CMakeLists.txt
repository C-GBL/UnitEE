# samples/08-texcache -- M3 acceptance: working set 3x the budget, verified.

add_executable(08-texcache main.cpp)

target_link_libraries(08-texcache PRIVATE
    ps2ur
    draw graph math3d packet dma kernel)
