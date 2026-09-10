# samples/24-dirty-launcher -- loads game.elf the way a launcher does, after
# filling free memory with a non-zero pattern (see main.cpp).
#
# It links at 0x01F00000, above every game, so that loading the target at
# 0x00100000 cannot overwrite it. The default ps2sdk linkfile hardcodes
# 0x00100000, so a copy with the address replaced is generated here and
# swapped into this directory's link flags.

if(NOT DEFINED PS2_LINKFILE)
    set(PS2_LINKFILE "${PS2SDK}/ee/startup/linkfile")
endif()
set(DIRTY_LINKFILE "${CMAKE_CURRENT_BINARY_DIR}/linkfile-high")
file(READ "${PS2_LINKFILE}" _linkfile_text)
string(REPLACE "0x00100000" "0x01F00000" _linkfile_text "${_linkfile_text}")
file(WRITE "${DIRTY_LINKFILE}" "${_linkfile_text}")
# Quoted, because this checkout path contains a space and the flag reaches
# the linker through a shell.
string(REPLACE "-T${PS2_LINKFILE}" "-T\"${DIRTY_LINKFILE}\""
       CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")

add_executable(24-dirty-launcher main.cpp)
target_link_libraries(24-dirty-launcher PRIVATE kernel)
