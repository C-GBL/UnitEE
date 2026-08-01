# samples/09-vu1-path1 -- M4 task 1: proves the VU1 -> XGKICK -> GS route.

add_executable(09-vu1-path1 main.cpp)

# The passthrough microprogram is compiled into ps2ur, but its blob symbols are
# only referenced from here, so the archive member must be kept.
target_link_libraries(09-vu1-path1 PRIVATE
    ps2ur
    draw graph math3d packet packet2 dma kernel)
