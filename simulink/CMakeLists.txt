# The Simulink sink block.
#
# Built through CMake rather than a hand-written build.m so the MEX inherits exactly the
# same link line as the library. That matters with a static libcurl: the full transitive
# set (zlib, ws2_32, crypt32, secur32, bcrypt, normaliz, ...) is known to CMake and
# maintaining a second copy of it by hand would drift.

find_package(Matlab COMPONENTS MX_LIBRARY MAIN_PROGRAM QUIET)

if(NOT Matlab_FOUND)
  message(STATUS "MATLAB not found — skipping the Simulink block. "
                 "Set -DMatlab_ROOT_DIR=<matlabroot> to build it.")
  return()
endif()

message(STATUS "MATLAB: ${Matlab_ROOT_DIR}")

# simstruc.h and the S-function machinery live outside the normal MEX include path.
set(NM_SIMULINK_INCLUDE "${Matlab_ROOT_DIR}/simulink/include")
if(NOT EXISTS "${NM_SIMULINK_INCLUDE}/simstruc.h")
  message(STATUS "No simstruc.h under ${NM_SIMULINK_INCLUDE} — Simulink may not be "
                 "installed; skipping the block.")
  return()
endif()

matlab_add_mex(
  NAME nominal_sink_sfun
  SRC src/nominal_sink_sfun.c
  OUTPUT_NAME nominal_sink_sfun
)

target_include_directories(nominal_sink_sfun PRIVATE "${NM_SIMULINK_INCLUDE}")

# A fault inside an S-function is reported as a bare access violation that takes MATLAB
# with it, naming no callback. -DNM_SFUN_TRACE=ON logs each one on entry.
option(NM_SFUN_TRACE "Log S-function callback entry and exit" OFF)
if(NM_SFUN_TRACE)
  target_compile_definitions(nominal_sink_sfun PRIVATE NM_SFUN_TRACE=1)
endif()

# Plain signature, not PRIVATE: matlab_add_mex already links the MEX libraries with the
# non-keyword form, and CMake refuses to mix the two on one target.
target_link_libraries(nominal_sink_sfun nominalstream)

# Land the built block in the blockset's mex/ folder, which is what ships in the toolbox.
set(NM_BLOCKSET_MEX "${CMAKE_CURRENT_SOURCE_DIR}/blockset/mex")
set_target_properties(nominal_sink_sfun PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY         "${NM_BLOCKSET_MEX}"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${NM_BLOCKSET_MEX}"
  LIBRARY_OUTPUT_DIRECTORY         "${NM_BLOCKSET_MEX}"
  LIBRARY_OUTPUT_DIRECTORY_RELEASE "${NM_BLOCKSET_MEX}"
)
