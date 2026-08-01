# samples/01-spinning-cube (plan section 9, M2 acceptance).
#
# The first sample that links ps2ur: it drives GsDevice's video mode, clear,
# double buffering, and immediate-mode triangle path.

add_executable(01-spinning-cube main.cpp)

target_link_libraries(01-spinning-cube PRIVATE
    ps2ur
    draw graph math3d packet dma kernel)
