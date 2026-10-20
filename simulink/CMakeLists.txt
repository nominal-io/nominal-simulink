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

# On macOS, matlab_add_mex exports the C++ MEX entry points from every MEX, this C one
# included. The block doesn't define them, and the current Xcode linker won't export a
# symbol that isn't there. Drop them, which leaves the same exports as mex's C build.
if(APPLE)
  get_target_property(_nm_link_flags nominal_sink_sfun LINK_FLAGS)
  string(REGEX REPLACE "-Wl,-U,_mex[A-Za-z]+" "" _nm_link_flags "${_nm_link_flags}")
  string(REGEX REPLACE "-Wl,-exported_symbols_list,[^ ]*cppMexFunction\\.map" ""
         _nm_link_flags "${_nm_link_flags}")
  set_target_properties(nominal_sink_sfun PROPERTIES LINK_FLAGS "${_nm_link_flags}")
endif()

target_include_directories(nominal_sink_sfun PRIVATE "${NM_SIMULINK_INCLUDE}")

# A fault inside an S-function is reported as a bare access violation that takes MATLAB
# with it, naming no callback. -DNM_SFUN_TRACE=ON logs entry to each callback.
option(NM_SFUN_TRACE "Log S-function callback entry" OFF)
if(NM_SFUN_TRACE)
  target_compile_definitions(nominal_sink_sfun PRIVATE NM_SFUN_TRACE=1)
endif()

# Plain signature, not PRIVATE: matlab_add_mex already links the MEX libraries with the
# non-keyword form, and CMake refuses to mix the two on one target.
target_link_libraries(nominal_sink_sfun nominalstream)

# The MEX is a shared library, so the static library linked into it has to be compiled
# as position-independent code. On Linux, GCC doesn't do that unless asked.
set_property(TARGET nominalstream PROPERTY POSITION_INDEPENDENT_CODE ON)

if(NOT WIN32)
  target_link_libraries(nominal_sink_sfun m)   # llround
endif()

# Land the built block in the blockset's mex/ folder, which is what ships in the toolbox.
set(NM_BLOCKSET_MEX "${CMAKE_CURRENT_SOURCE_DIR}/blockset/mex")
set_target_properties(nominal_sink_sfun PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY         "${NM_BLOCKSET_MEX}"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${NM_BLOCKSET_MEX}"
  LIBRARY_OUTPUT_DIRECTORY         "${NM_BLOCKSET_MEX}"
  LIBRARY_OUTPUT_DIRECTORY_RELEASE "${NM_BLOCKSET_MEX}"
)
