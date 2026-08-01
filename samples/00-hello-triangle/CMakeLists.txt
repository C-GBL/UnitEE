# samples/00-hello-triangle (plan section 9, M0 task 5).
#
# Depends only on ps2sdk, not on ps2ur: this sample must keep booting even
# while the runtime is mid-refactor, because it is the canary that tells you
# whether the toolchain or your code broke.

add_executable(00-hello-triangle main.c)

# ps2sdk libraries for the GS path. draw/graph/packet/dma are what M0 needs;
# math3d and kernel come along for the sample's SleepThread and future use.
target_link_libraries(00-hello-triangle PRIVATE
    draw graph math3d packet dma kernel)

# CMAKE_EXECUTABLE_SUFFIX is .elf (set by the toolchain file), so this produces
# 00-hello-triangle.elf.
