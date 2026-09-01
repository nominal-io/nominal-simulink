# Cross-compile for QNX Neutrino 7.1, x86-64 — the RTOS behind Simulink Real-Time
# R2021a through R2025b.
#
# Usage:
#   source $QNX_SDP/qnxsdp-env.sh          (sets QNX_HOST and QNX_TARGET)
#   cmake -S . -B build-qnx \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-qnx710-x86_64.cmake
#
# The compiler is qcc, QNX's driver around gcc. It must match the version bundled with
# the Simulink Real-Time Target Support Package — 7.1.0 for R2021a onward, which is
# constant across that whole release range, so this is one toolchain rather than five.

set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_VERSION 7.1.0)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED ENV{QNX_HOST} OR NOT DEFINED ENV{QNX_TARGET})
  message(FATAL_ERROR
    "QNX_HOST and QNX_TARGET are not set. Source qnxsdp-env.sh from the QNX SDP first.")
endif()

set(QNX_HOST   "$ENV{QNX_HOST}")
set(QNX_TARGET "$ENV{QNX_TARGET}")

# -Vgcc_nto<arch> selects the target variant. x86_64 uses the `o` suffix convention.
set(QNX_COMPILER_TARGET gcc_ntox86_64)

if(CMAKE_HOST_WIN32)
  set(QNX_EXE_SUFFIX ".exe")
else()
  set(QNX_EXE_SUFFIX "")
endif()

set(CMAKE_C_COMPILER   "${QNX_HOST}/usr/bin/qcc${QNX_EXE_SUFFIX}")
set(CMAKE_CXX_COMPILER "${QNX_HOST}/usr/bin/q++${QNX_EXE_SUFFIX}")
set(CMAKE_AR           "${QNX_HOST}/usr/bin/ntox86_64-ar${QNX_EXE_SUFFIX}"     CACHE FILEPATH "")
set(CMAKE_RANLIB       "${QNX_HOST}/usr/bin/ntox86_64-ranlib${QNX_EXE_SUFFIX}" CACHE FILEPATH "")

set(CMAKE_C_COMPILER_TARGET   ${QNX_COMPILER_TARGET})
set(CMAKE_CXX_COMPILER_TARGET ${QNX_COMPILER_TARGET})

# qcc needs -V on every invocation, link included, or it drives the host compiler.
set(CMAKE_C_FLAGS_INIT   "-V${QNX_COMPILER_TARGET}")
set(CMAKE_CXX_FLAGS_INIT "-V${QNX_COMPILER_TARGET}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-V${QNX_COMPILER_TARGET}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-V${QNX_COMPILER_TARGET}")

set(CMAKE_SYSROOT "${QNX_TARGET}")
set(CMAKE_FIND_ROOT_PATH "${QNX_TARGET}/x86_64" "${QNX_TARGET}")

# Look for programs on the host, but headers and libraries only in the sysroot --
# otherwise a host libcurl gets picked up and the link fails confusingly late.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Static by default: Simulink Real-Time packs .a files into the real-time application
# itself, which removes any need to install anything on the target.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
