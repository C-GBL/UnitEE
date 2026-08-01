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

# add_vu_program(<target> SOURCES <vsm>...)   [plan section 9, M1 task 3]
#
# Assembles VU microprogram sources with dvp-as and attaches the resulting
# objects to <target>, so the microcode is embedded in the EE ELF and can be
# DMA'd to VU1 micro memory at load time (plan sections 3.2, 3.4; M4 task 1).
#
# DEVIATION from the plan's wording, verified against the toolchain 2026-07-31.
# M1 task 3 asks this function to "convert the output to a linkable object with
# symbols <name>_start/<name>_end (use ld -r -b binary or an .incbin stub)".
# That post-processing is unnecessary: dvp-as already emits a directly linkable
# EE object with .vutext/.vudata/.vubss sections and whatever global symbols
# the source declares. Assembling ps2sdk's own samples/draw/vu1/draw_3D.vsm
# yields:
#
#     00000000 T VU1Draw3D_CodeStart
#     00000180 T VU1Draw3D_CodeEnd     (0x180 = 24 instructions x 16 bytes)
#
# So the symbols come from the .vsm SOURCE, not from this function. That is the
# ps2sdk convention and it is what packet2_vif_add_micro_program(pkt, 0,
# &Start, &End) expects, so following it keeps us compatible with the SDK
# helpers we use at M4. Adding an .incbin indirection would only rename symbols
# that are already correct.
#
# CONTRACT for .vsm authors: every microprogram must declare
#
#     .vu
#     .align 4
#     .global <Name>_CodeStart
#     .global <Name>_CodeEnd
#
# and the C++ side declares them as
#
#     extern "C" u32 <Name>_CodeStart __attribute__((section(".vudata")));
#     extern "C" u32 <Name>_CodeEnd   __attribute__((section(".vudata")));
function(add_vu_program target)
  cmake_parse_arguments(_VU "" "" "SOURCES" ${ARGN})

  if(NOT TARGET "${target}")
    message(FATAL_ERROR "add_vu_program: '${target}' is not a target")
  endif()
  if(NOT _VU_SOURCES)
    message(FATAL_ERROR "add_vu_program(${target}): no SOURCES given")
  endif()
  if(NOT EXISTS "${PS2_DVP_AS}")
    message(FATAL_ERROR
      "add_vu_program(${target}): dvp-as not found at '${PS2_DVP_AS}'. "
      "Install the toolchain (tools/ps2dev/install.ps1) or set PS2DEV.")
  endif()

  set(_vu_dir "${CMAKE_CURRENT_BINARY_DIR}/vu")
  file(MAKE_DIRECTORY "${_vu_dir}")

  foreach(_src IN LISTS _VU_SOURCES)
    get_filename_component(_src_abs "${_src}" ABSOLUTE)
    get_filename_component(_src_we "${_src}" NAME_WE)
    set(_obj "${_vu_dir}/${_src_we}.vsm.o")

    # dvp-as wants the source before -o; it assembles one file at a time.
    add_custom_command(
      OUTPUT "${_obj}"
      COMMAND "${PS2_DVP_AS}" "${_src_abs}" -o "${_obj}"
      DEPENDS "${_src_abs}"
      COMMENT "dvp-as ${_src_we}.vsm"
      VERBATIM)

    set_source_files_properties("${_obj}" PROPERTIES
      EXTERNAL_OBJECT TRUE
      GENERATED TRUE)
    target_sources("${target}" PRIVATE "${_obj}")
  endforeach()
endfunction()

# Single-source spelling, kept because it reads better at one call site.
function(add_vu_microprogram target vsm_source)
  add_vu_program("${target}" SOURCES "${vsm_source}")
endfunction()
