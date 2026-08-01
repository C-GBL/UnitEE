# tools/cmake/vu.cmake
#
# Build rules for VU microprograms (.vsm), assembled with dvp-as and linked
# into the EE ELF as data blobs that are DMA'd to VU1 micro memory at load
# time (plan section 3.2).
#
# Usage:
#   include("${CMAKE_CURRENT_LIST_DIR}/vu.cmake")   # or via CMAKE_MODULE_PATH
#   add_vu_microprogram(ps2ur "${CMAKE_CURRENT_SOURCE_DIR}/src/vu/vu_unlit.vsm")

# Resolve PS2DEV the same way as ps2-toolchain.cmake (env var, then default),
# in case this module is included without the toolchain file (host builds
# never assemble VU code, but the include must still be harmless).
if(NOT DEFINED PS2DEV OR "${PS2DEV}" STREQUAL "")
  if(DEFINED ENV{PS2DEV} AND NOT "$ENV{PS2DEV}" STREQUAL "")
    set(PS2DEV "$ENV{PS2DEV}")
  else()
    set(PS2DEV "C:/Users/Ash/ps2dev")
  endif()
  file(TO_CMAKE_PATH "${PS2DEV}" PS2DEV)
endif()

set(_vu_tool_suffix "")
if(CMAKE_HOST_WIN32)
  set(_vu_tool_suffix ".exe")
endif()

# Prefer the toolchain install location; fall back to dvp-as on PATH.
set(PS2_DVP_AS "${PS2DEV}/dvp/bin/dvp-as${_vu_tool_suffix}")
if(NOT EXISTS "${PS2_DVP_AS}")
  find_program(PS2_DVP_AS_ON_PATH NAMES dvp-as)
  if(PS2_DVP_AS_ON_PATH)
    set(PS2_DVP_AS "${PS2_DVP_AS_ON_PATH}")
  endif()
endif()

# add_vu_microprogram(<target> <vsm_source>)
#
# Assembles <vsm_source> (a .vsm VU assembly file) with dvp-as into an object
# file and attaches that object to <target> so the microprogram is embedded
# in the final ELF.
#
# TODO(verify dvp-as output embedding strategy): once the toolchain is
# installed, confirm whether dvp-as emits a directly linkable EE ELF object
# with usable start/end symbols, or whether the output must be post-processed
# (e.g. objcopy into .vudata, or bin2c) so the runtime's microprogram manager
# can find the blob by symbol name. The command below assumes the simple
# "dvp-as -o out.o in.vsm" form used by ps2sdk samples.
function(add_vu_microprogram target vsm_source)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "add_vu_microprogram: '${target}' is not a target")
  endif()

  get_filename_component(_vu_src_abs "${vsm_source}" ABSOLUTE)
  get_filename_component(_vu_name "${vsm_source}" NAME_WE)
  set(_vu_obj_dir "${CMAKE_CURRENT_BINARY_DIR}/vu")
  set(_vu_obj "${_vu_obj_dir}/${_vu_name}.vsm.o")

  file(MAKE_DIRECTORY "${_vu_obj_dir}")

  add_custom_command(
    OUTPUT "${_vu_obj}"
    COMMAND "${PS2_DVP_AS}" -o "${_vu_obj}" "${_vu_src_abs}"
    DEPENDS "${_vu_src_abs}"
    COMMENT "dvp-as ${_vu_name}.vsm"
    VERBATIM
  )

  set_source_files_properties("${_vu_obj}" PROPERTIES
    EXTERNAL_OBJECT TRUE
    GENERATED TRUE
  )
  target_sources("${target}" PRIVATE "${_vu_obj}")
endfunction()
